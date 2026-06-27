#!/usr/bin/env python3
"""Wikore test corpus fetcher.

Downloads a manifest of public documents (SEC EDGAR, arXiv, EU Open Data,
NIST, data.gov, GitHub OSS, Project Gutenberg) into
``tests/fixtures/corpus/files/`` for end-to-end ingest testing.

Design constraints
------------------

* **Stdlib only** - no `requests` dep. Wikore's Python footprint stays at
  zero third-party packages so the test corpus build runs anywhere
  Python 3.10+ is available.
* **Idempotent** - re-running skips already-downloaded files whose
  observed SHA-256 matches the manifest entry. Safe to interrupt and
  resume.
* **Rate-limited per host** - SEC EDGAR enforces 10 req/s; arXiv asks
  for similar etiquette. Per-host token bucket prevents accidental
  abuse.
* **Polite User-Agent** - every request carries an identifying UA
  with a contact URL; SEC EDGAR rejects anonymous bulk requests.
* **License-aware** - every manifest entry has a ``license`` tag.
  ``--require-license`` filter lets CI assert that only certain
  licenses are pulled (e.g. for the public-facing demo corpus).
* **Tenant-partitioned** - every manifest entry has a
  ``company_tag`` and ``org_unit_slug`` so the downstream ingest test
  knows which synthetic tenant to attach the document to.

Usage
-----

Download the first 100 entries (typical iteration test):

    python scripts/corpus/fetch.py \
        --manifest tests/fixtures/corpus/seed_manifest.json \
        --output tests/fixtures/corpus/files \
        --limit 100

Restrict to a file-type subset (e.g. when validating a new PDF parser):

    python scripts/corpus/fetch.py --types pdf --limit 50

Dry-run (validate manifest, no downloads):

    python scripts/corpus/fetch.py --dry-run

Hash freeze (record observed SHA-256 of newly-downloaded files into a
sidecar so manifests stay reproducible):

    python scripts/corpus/fetch.py --freeze-hashes new_hashes.json
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import hashlib
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# --- Constants ------------------------------------------------------------

# Default UA. Replace the email/URL with your own for production fetches - 
# SEC EDGAR specifically asks for a contact email in the UA string.
DEFAULT_USER_AGENT = (
    "wikore-corpus-fetcher/0.1 "
    "(test corpus build; https://github.com/localrun-ai/wikore)"
)

# Per-host rate limits in requests per second. SEC EDGAR's published cap
# is 10/sec; we leave headroom. arXiv asks for 1 req every 3 seconds for
# bulk access via their OAI API. Other hosts are gentler but bounded
# anyway to avoid bursty patterns.
DEFAULT_RATE_LIMITS = {
    "www.sec.gov": 8.0,
    "export.arxiv.org": 1.0,
    "arxiv.org": 1.0,
    "data.europa.eu": 4.0,
    "raw.githubusercontent.com": 4.0,
    "nvlpubs.nist.gov": 4.0,
    "www.gutenberg.org": 2.0,
    "_default_": 4.0,
}

VALID_FILE_TYPES = {"pdf", "docx", "pptx", "xlsx", "xls", "md", "txt", "html"}

# --- Manifest schema ------------------------------------------------------


@dataclasses.dataclass
class ManifestEntry:
    url: str
    filename: str
    file_type: str
    license: str
    source: str
    company_tag: str
    org_unit_slug: str
    sensitivity_default: str = "internal"
    sha256: str | None = None     # may be empty before first fetch
    size_bytes: int | None = None
    notes: str = ""

    @classmethod
    def from_dict(cls, d: dict) -> "ManifestEntry":
        # Enforce required keys with friendly error if missing.
        required = {"url", "filename", "file_type", "license", "source",
                    "company_tag", "org_unit_slug"}
        missing = required - d.keys()
        if missing:
            raise ValueError(f"manifest entry missing keys: {missing} - {d!r}")
        if d["file_type"] not in VALID_FILE_TYPES:
            raise ValueError(
                f"file_type {d['file_type']!r} not in {sorted(VALID_FILE_TYPES)}"
            )
        return cls(
            url=d["url"],
            filename=d["filename"],
            file_type=d["file_type"],
            license=d["license"],
            source=d["source"],
            company_tag=d["company_tag"],
            org_unit_slug=d["org_unit_slug"],
            sensitivity_default=d.get("sensitivity_default", "internal"),
            sha256=d.get("sha256") or None,
            size_bytes=d.get("size_bytes"),
            notes=d.get("notes", ""),
        )

    @property
    def dest_subdir(self) -> str:
        # files/{company_tag}/{org_unit_slug}/{filename}
        return f"{self.company_tag}/{self.org_unit_slug}"


# --- Rate limiter ---------------------------------------------------------


class HostRateLimiter:
    """Per-host token bucket. Thread-safe."""

    def __init__(self, rate_limits: dict[str, float]):
        self._rates = rate_limits
        self._last_request: dict[str, float] = {}
        self._lock = threading.Lock()

    def wait(self, host: str) -> None:
        rate = self._rates.get(host, self._rates.get("_default_", 4.0))
        min_interval = 1.0 / rate
        # Reserve the slot before releasing the lock: the next allowed
        # request for this host is now + wait_for, so we write that back
        # before any sleep. This prevents the TOCTOU race where two threads
        # for the same host read the same `last`, both compute the same
        # wait, both sleep in parallel, and then both fire simultaneously.
        with self._lock:
            now = time.monotonic()
            last = self._last_request.get(host, 0.0)
            scheduled = max(now, last + min_interval)
            self._last_request[host] = scheduled
            wait_for = scheduled - now
        if wait_for > 0:
            time.sleep(wait_for)


# --- Core fetch logic -----------------------------------------------------


@dataclasses.dataclass
class FetchResult:
    entry: ManifestEntry
    dest: Path
    status: str   # 'downloaded' | 'cached' | 'hash_mismatch' | 'error' | 'skipped'
    bytes_written: int = 0
    observed_sha256: str | None = None
    error: str | None = None


def _compute_sha256(path: Path, chunk_size: int = 1 << 16) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _http_get(url: str, user_agent: str, timeout: float,
              retries: int = 3, backoff: float = 1.5) -> bytes:
    last_exc: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            req = urllib.request.Request(
                url,
                headers={"User-Agent": user_agent, "Accept": "*/*"},
            )
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.read()
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as e:
            last_exc = e
            if attempt < retries:
                time.sleep(backoff ** attempt)
    raise RuntimeError(f"GET {url} failed after {retries} attempts: {last_exc}")


def _fetch_one(entry: ManifestEntry, output_root: Path,
               rate_limiter: HostRateLimiter,
               user_agent: str, timeout: float,
               verify_hash: bool) -> FetchResult:
    dest_dir = output_root / entry.dest_subdir
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / entry.filename

    # Idempotency: if file exists and the manifest carries an expected hash
    # that matches, skip the download entirely.
    if dest.exists():
        if verify_hash and entry.sha256:
            observed = _compute_sha256(dest)
            if observed == entry.sha256:
                return FetchResult(entry, dest, "cached",
                                   bytes_written=dest.stat().st_size,
                                   observed_sha256=observed)
            else:
                # Stale or partial; re-download.
                dest.unlink()
        else:
            # No expected hash - trust that file is OK if it exists.
            return FetchResult(entry, dest, "cached",
                               bytes_written=dest.stat().st_size,
                               observed_sha256=_compute_sha256(dest))

    host = urllib.parse.urlparse(entry.url).hostname or ""
    rate_limiter.wait(host)

    try:
        data = _http_get(entry.url, user_agent=user_agent, timeout=timeout)
    except Exception as e:
        return FetchResult(entry, dest, "error", error=str(e))

    # Atomic write: write to .part then rename.
    # Hash is computed from the in-memory bytes (not by re-reading the
    # file) since we already have them; saves a disk round-trip.
    observed = hashlib.sha256(data).hexdigest()
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(data)

    if verify_hash and entry.sha256 and observed != entry.sha256:
        tmp.unlink()
        return FetchResult(
            entry, dest, "hash_mismatch",
            observed_sha256=observed,
            error=f"expected {entry.sha256[:16]}.., got {observed[:16]}..",
        )

    tmp.rename(dest)
    return FetchResult(entry, dest, "downloaded",
                       bytes_written=len(data),
                       observed_sha256=observed)


# --- Top-level orchestration ----------------------------------------------


def _load_manifest(path: Path) -> list[ManifestEntry]:
    with path.open() as f:
        raw = json.load(f)
    if not isinstance(raw, list):
        raise ValueError(f"manifest {path}: expected JSON list, got {type(raw)}")
    return [ManifestEntry.from_dict(d) for d in raw]


def _filter_entries(entries: list[ManifestEntry],
                    types: set[str] | None,
                    companies: set[str] | None,
                    licenses: set[str] | None,
                    limit: int | None) -> list[ManifestEntry]:
    out = entries
    if types:
        out = [e for e in out if e.file_type in types]
    if companies:
        out = [e for e in out if e.company_tag in companies]
    if licenses:
        out = [e for e in out if e.license in licenses]
    if limit is not None:
        out = out[:limit]
    return out


def _print_summary(results: list[FetchResult]) -> int:
    by_status: dict[str, int] = {}
    bytes_total = 0
    for r in results:
        by_status[r.status] = by_status.get(r.status, 0) + 1
        bytes_total += r.bytes_written
    print()
    print(f"-- {len(results)} entries processed; {bytes_total / 1e6:.1f} MB total")
    for status in ("downloaded", "cached", "hash_mismatch", "error", "skipped"):
        if by_status.get(status):
            print(f"   {status:>14}: {by_status[status]}")
    errors = [r for r in results if r.status in ("error", "hash_mismatch")]
    if errors:
        print("\n-- Failures:")
        for r in errors[:10]:
            print(f"   {r.entry.filename}: {r.error}")
        if len(errors) > 10:
            print(f"   ... and {len(errors) - 10} more")
    return 0 if not errors else 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--manifest", type=Path,
                   default=Path("tests/fixtures/corpus/seed_manifest.json"),
                   help="manifest JSON path (default: seed_manifest.json)")
    p.add_argument("--output", type=Path,
                   default=Path("tests/fixtures/corpus/files"),
                   help="output root directory")
    p.add_argument("--limit", type=int,
                   help="stop after N entries (post-filter)")
    p.add_argument("--types", type=str,
                   help="comma-separated file types to keep (e.g. pdf,docx)")
    p.add_argument("--companies", type=str,
                   help="comma-separated company_tag filter (e.g. acme,beta)")
    p.add_argument("--require-license", type=str,
                   help="comma-separated allow-list of license tags")
    p.add_argument("--user-agent", default=DEFAULT_USER_AGENT)
    p.add_argument("--timeout", type=float, default=30.0,
                   help="per-request timeout seconds")
    p.add_argument("--parallel", type=int, default=4,
                   help="max concurrent downloads (rate-limited per host)")
    p.add_argument("--dry-run", action="store_true",
                   help="validate manifest + print plan; no downloads")
    p.add_argument("--no-verify-hash", action="store_true",
                   help="skip SHA-256 verification (use for first-time fetch of "
                        "freshly-curated manifests; pair with --freeze-hashes)")
    p.add_argument("--freeze-hashes", type=Path,
                   help="write observed SHA-256 for each downloaded file to "
                        "this JSON file (use to bake hashes into the manifest)")

    args = p.parse_args(argv)

    if not args.manifest.exists():
        print(f"manifest not found: {args.manifest}", file=sys.stderr)
        return 2

    entries = _load_manifest(args.manifest)

    types = set(args.types.split(",")) if args.types else None
    companies = set(args.companies.split(",")) if args.companies else None
    licenses = set(args.require_license.split(",")) if args.require_license else None

    selected = _filter_entries(entries, types, companies, licenses, args.limit)

    print(f"-- Manifest: {args.manifest} ({len(entries)} entries total)")
    print(f"-- Selected: {len(selected)} entries after filters")
    if types: print(f"     types={sorted(types)}")
    if companies: print(f"     companies={sorted(companies)}")
    if licenses: print(f"     licenses={sorted(licenses)}")
    if args.limit: print(f"     limit={args.limit}")

    if args.dry_run:
        print("-- Dry run: no downloads.")
        # Print first few entries as a sanity check
        for e in selected[:5]:
            print(f"   would fetch: [{e.file_type:>5}] {e.url}  -> {e.dest_subdir}/{e.filename}")
        if len(selected) > 5:
            print(f"   ... and {len(selected) - 5} more")
        return 0

    args.output.mkdir(parents=True, exist_ok=True)
    rate_limiter = HostRateLimiter(DEFAULT_RATE_LIMITS)
    results: list[FetchResult] = []

    print(f"-- Fetching to {args.output}/ (parallel={args.parallel})")
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel) as ex:
        futures = {
            ex.submit(_fetch_one, e, args.output, rate_limiter,
                      args.user_agent, args.timeout,
                      not args.no_verify_hash): e
            for e in selected
        }
        for i, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            result = fut.result()
            results.append(result)
            sym = {"downloaded": "+", "cached": ".", "error": "!",
                   "hash_mismatch": "X", "skipped": "-"}[result.status]
            print(f"   [{i:>4}/{len(selected)}] {sym} {result.entry.filename} "
                  f"({result.status}{', '+str(result.bytes_written)+'B' if result.bytes_written else ''})")

    if args.freeze_hashes:
        observed = {
            r.entry.filename: r.observed_sha256
            for r in results
            if r.observed_sha256 and r.status in ("downloaded", "cached")
        }
        args.freeze_hashes.write_text(json.dumps(observed, indent=2, sort_keys=True))
        print(f"-- Wrote {len(observed)} observed hashes to {args.freeze_hashes}")

    return _print_summary(results)


if __name__ == "__main__":
    sys.exit(main())
