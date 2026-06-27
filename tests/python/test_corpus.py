"""Tests for the test-corpus tooling (fetcher + negative-sample generator).

Run from the repo root:

    python3 -m unittest tests.python.test_corpus -v

Or:

    python3 tests/python/test_corpus.py

These tests are stdlib-only (no pytest dependency) to match the scripts
they exercise. They never touch the network: the fetcher is exercised
only via its filter helpers and --dry-run path.
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import sys
import unittest
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


fetch = _load_module(
    "wikore_corpus_fetch",
    REPO_ROOT / "scripts/corpus/fetch.py",
)
generate = _load_module(
    "wikore_corpus_generate",
    REPO_ROOT / "scripts/corpus/generate_negatives.py",
)

SEED_MANIFEST = REPO_ROOT / "tests/fixtures/corpus/seed_manifest.json"
NEG_MANIFEST  = REPO_ROOT / "tests/fixtures/corpus/negative_manifest.json"


# ---------------------------------------------------------------------------
# Seed manifest schema tests
# ---------------------------------------------------------------------------


class SeedManifestSchema(unittest.TestCase):
    def setUp(self):
        with SEED_MANIFEST.open() as f:
            self.entries = json.load(f)

    def test_loads_as_list(self):
        self.assertIsInstance(self.entries, list)
        self.assertGreater(len(self.entries), 0)

    def test_every_entry_parses(self):
        for d in self.entries:
            fetch.ManifestEntry.from_dict(d)

    def test_file_types_in_allowed_set(self):
        for d in self.entries:
            self.assertIn(d["file_type"], fetch.VALID_FILE_TYPES,
                          f"{d['filename']}: {d['file_type']!r} not allowed")

    def test_company_tag_in_known_set(self):
        for d in self.entries:
            self.assertIn(d["company_tag"], {"acme", "beta", "gamma"})

    def test_filenames_unique(self):
        names = [d["filename"] for d in self.entries]
        dups = [n for n in names if names.count(n) > 1]
        self.assertFalse(dups, f"duplicate filenames: {sorted(set(dups))}")

    def test_license_field_present_and_nonempty(self):
        for d in self.entries:
            self.assertIsInstance(d.get("license"), str)
            self.assertTrue(d["license"], f"empty license on {d['filename']}")

    def test_urls_well_formed(self):
        for d in self.entries:
            url = d["url"]
            self.assertTrue(
                url.startswith("https://") or url.startswith("http://"),
                f"bad URL on {d['filename']}: {url}",
            )


# ---------------------------------------------------------------------------
# Negative manifest schema tests
# ---------------------------------------------------------------------------


class NegativeManifestSchema(unittest.TestCase):
    def setUp(self):
        with NEG_MANIFEST.open() as f:
            self.entries = json.load(f)

    def test_loads_as_list(self):
        self.assertIsInstance(self.entries, list)
        self.assertGreater(len(self.entries), 0)

    def test_required_fields_present(self):
        required = {"filename", "generator", "failure_class", "category",
                    "expected_behaviour", "sha256", "size_bytes", "notes"}
        for d in self.entries:
            missing = required - d.keys()
            self.assertFalse(missing,
                             f"missing fields on {d.get('filename')!r}: {missing}")

    def test_categories_constrained(self):
        allowed = {"malformed", "dangerous", "prompt-injection"}
        for d in self.entries:
            self.assertIn(d["category"], allowed,
                          f"{d['filename']}: bad category {d['category']!r}")

    def test_all_16_failure_classes_present(self):
        expected_classes = {
            "empty-file", "truncated-file", "mime-mismatch", "malformed-xml",
            "missing-required-part", "zip-slip", "xxe-external-entity",
            "xml-bomb", "oversized-attribute", "malware-eicar-signature",
            "pdf-javascript-action", "office-macros",
            "prompt-injection-text", "prompt-injection-hidden-html",
            "prompt-injection-unicode-tags", "prompt-injection-exfil-link",
        }
        seen = {d["failure_class"] for d in self.entries}
        self.assertEqual(seen, expected_classes,
                         f"missing: {expected_classes - seen}; "
                         f"extra: {seen - expected_classes}")

    def test_every_generator_resolves(self):
        for d in self.entries:
            self.assertIn(d["generator"], generate.GENERATORS,
                          f"unknown generator {d['generator']!r} "
                          f"for {d['filename']}")

    def test_sha256_is_present_and_hex(self):
        for d in self.entries:
            sha = d.get("sha256")
            self.assertIsInstance(sha, str,
                                  f"{d['filename']}: sha256 must be a hex "
                                  f"string (run --freeze to populate)")
            self.assertEqual(len(sha), 64,
                             f"{d['filename']}: sha256 must be 64 hex chars")
            int(sha, 16)  # raises ValueError if non-hex

    def test_size_bytes_is_int(self):
        for d in self.entries:
            self.assertIsInstance(d.get("size_bytes"), int,
                                  f"{d['filename']}: size_bytes must be int")


# ---------------------------------------------------------------------------
# Generator determinism: regenerate every sample and assert byte equality
# ---------------------------------------------------------------------------


class GeneratorDeterminism(unittest.TestCase):
    """Critical: each generator must produce byte-identical output every run.

    Failure means the corpus stops being reproducible across machines and
    CI cannot enforce expected SHA-256 values from the manifest.
    """

    def setUp(self):
        with NEG_MANIFEST.open() as f:
            self.entries = json.load(f)

    def test_sha256_matches_manifest(self):
        for entry in self.entries:
            with self.subTest(filename=entry["filename"]):
                fn = generate.GENERATORS[entry["generator"]]
                data = fn()
                observed = hashlib.sha256(data).hexdigest()
                self.assertEqual(
                    observed, entry["sha256"],
                    f"{entry['filename']}: generator produced sha256 "
                    f"{observed} but manifest expects {entry['sha256']}. "
                    f"Run scripts/corpus/generate_negatives.py --freeze "
                    f"to update.",
                )

    def test_size_matches_manifest(self):
        for entry in self.entries:
            with self.subTest(filename=entry["filename"]):
                data = generate.GENERATORS[entry["generator"]]()
                self.assertEqual(len(data), entry["size_bytes"])

    def test_running_twice_produces_identical_output(self):
        # Catch any nondeterminism that might slip into a future generator
        # (unsorted dict iteration, time-based padding, random salt, etc.)
        for entry in self.entries:
            with self.subTest(filename=entry["filename"]):
                fn = generate.GENERATORS[entry["generator"]]
                self.assertEqual(fn(), fn())


# ---------------------------------------------------------------------------
# Specific negative-sample invariants
# ---------------------------------------------------------------------------


class NegativeSampleInvariants(unittest.TestCase):
    """Smoke checks on individual samples to catch generator regressions."""

    # EICAR
    EICAR_CANONICAL_SHA256 = (
        "275a021bbfb6489e54d471899f7db9d1663fc695ec2fe2a2c4538aabf651fd0f"
    )

    def test_eicar_signature_byte_perfect(self):
        eicar = generate.eicar_txt()
        self.assertEqual(len(eicar), 68, "EICAR signature must be exactly 68 bytes")
        self.assertTrue(eicar.startswith(b"X5O!P%@AP[4\\PZX54(P^)7CC)7}"))
        self.assertTrue(eicar.endswith(b"$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*"))

    def test_eicar_signature_well_known_sha256(self):
        # https://www.eicar.org/?page_id=3950 publishes this canonical hash.
        self.assertEqual(
            hashlib.sha256(generate.eicar_txt()).hexdigest(),
            self.EICAR_CANONICAL_SHA256,
        )

    def test_empty_files_are_empty(self):
        self.assertEqual(generate.empty_pdf(), b"")
        self.assertEqual(generate.empty_docx(), b"")

    def test_truncated_pdf_has_magic_but_no_eof(self):
        data = generate.truncated_pdf()
        self.assertTrue(data.startswith(b"%PDF-"))
        self.assertNotIn(b"%%EOF", data,
                         "truncated PDF must not contain a valid EOF marker")

    def test_mime_mismatch_pdf_is_actually_a_zip(self):
        data = generate.mime_mismatch_pdf()
        # Zip magic, NOT PDF magic. Magic-byte sniff catches this.
        self.assertTrue(data.startswith(b"PK\x03\x04"))
        self.assertFalse(data.startswith(b"%PDF"))

    def test_malformed_xml_docx_has_unclosed_tag(self):
        data = generate.malformed_xml_docx()
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            doc = zf.read("word/document.xml")
        # The body has <w:t>...without a closing </w:t>; structural mismatch.
        # We assert <w:t> appears but its corresponding closer does not.
        self.assertIn(b"<w:t>", doc)
        self.assertNotIn(b"</w:t>", doc)

    def test_missing_part_docx_omits_document_xml(self):
        data = generate.missing_part_docx()
        names = zipfile.ZipFile(io.BytesIO(data)).namelist()
        self.assertNotIn("word/document.xml", names)
        self.assertIn("[Content_Types].xml", names)  # but the envelope is otherwise valid

    def test_zipslip_contains_traversal_entry(self):
        data = generate.zipslip_docx()
        names = zipfile.ZipFile(io.BytesIO(data)).namelist()
        self.assertTrue(any(".." in n for n in names),
                        f"expected path-traversal entry, got {names}")

    def test_xxe_docx_declares_external_entity(self):
        data = generate.xxe_docx()
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            doc = zf.read("word/document.xml")
        self.assertIn(b"<!ENTITY", doc)
        self.assertIn(b"SYSTEM", doc)

    def test_xml_bomb_has_deep_nesting(self):
        data = generate.xml_bomb_docx()
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            doc = zf.read("word/document.xml")
        # ~2000 levels of <n> in the body; counting <n> opens should be high.
        opens = doc.count(b"<n>")
        self.assertGreater(opens, 1000,
                           f"expected deep <n> nesting, got {opens} opens")

    def test_oversized_attribute_xlsx_has_huge_attribute(self):
        data = generate.oversized_attribute_xlsx()
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            sheet = zf.read("xl/worksheets/sheet1.xml")
        # The attribute should contain a long run of 'A' characters.
        self.assertGreater(sheet.count(b"A"), 100_000,
                           "expected an attribute value with >100k bytes")

    def test_macro_docx_contains_vba_project_entry(self):
        data = generate.docx_with_macro()
        names = zipfile.ZipFile(io.BytesIO(data)).namelist()
        self.assertIn("word/vbaProject.bin", names)

    def test_pdf_with_js_declares_js_action(self):
        data = generate.pdf_with_javascript()
        # /JS or /JavaScript token must be present in the PDF body.
        self.assertIn(b"/JS", data)
        self.assertIn(b"/JavaScript", data)
        self._assert_pdf_xref_offsets_are_correct(data)

    def test_eicar_in_pdf_has_pdf_magic_and_eicar_payload(self):
        data = generate.eicar_in_pdf()
        self.assertTrue(data.startswith(b"%PDF-"))
        # EICAR ascii payload appears verbatim inside the PDF stream.
        self.assertIn(b"$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*", data)
        self._assert_pdf_xref_offsets_are_correct(data)

    def _assert_pdf_xref_offsets_are_correct(self, data: bytes) -> None:
        """Confirm every offset in the xref table actually points at an
        `N 0 obj` line. Strict PDF parsers (the realistic adversary) follow
        the xref table; if these offsets are wrong they bail before our
        AV / JS-detection code can ever see the file. PR #17 review caught
        this exact bug class with hardcoded offsets.
        """
        import re
        # startxref must point at the byte position of "xref"
        m = re.search(rb"startxref\s*(\d+)\s*%%EOF", data)
        self.assertIsNotNone(m, "missing startxref/%%EOF")
        xref_start = int(m.group(1))
        self.assertEqual(data[xref_start:xref_start + 4], b"xref",
                         f"startxref={xref_start} does not point at 'xref' "
                         f"(found {data[xref_start:xref_start+8]!r})")

        # Parse "xref\n0 N\n" header
        xref_block = data[xref_start:]
        header = re.match(rb"xref\s*\n0\s+(\d+)\s*\n", xref_block)
        self.assertIsNotNone(header, "malformed xref header")
        n_entries = int(header.group(1))

        # Entry 0 is the head-of-free-list (offset 0, gen 65535, 'f').
        # Entries 1..N-1 each have offset, generation, and 'n'.
        # Each entry is exactly 20 bytes per ISO 32000-1 7.5.4, ending in
        # either CR+LF or SP+LF.
        body_start = xref_start + header.end()
        entry_re = re.compile(rb"(\d{10}) (\d{5}) n[ \r]\n")
        for i in range(1, n_entries):
            entry = data[body_start + i * 20: body_start + (i + 1) * 20]
            m2 = entry_re.match(entry)
            self.assertIsNotNone(m2, f"xref entry {i} malformed: {entry!r}")
            offset = int(m2.group(1))
            expected_marker = f"{i} 0 obj".encode()
            self.assertEqual(
                data[offset:offset + len(expected_marker)],
                expected_marker,
                f"xref entry {i} offset {offset} does not point at "
                f"'{expected_marker.decode()}' "
                f"(found {data[offset:offset+16]!r})",
            )

    def test_prompt_inject_basic_has_inject_marker(self):
        data = generate.prompt_inject_basic_md()
        self.assertIn(b"IGNORE ALL PREVIOUS INSTRUCTIONS", data)

    def test_prompt_inject_hidden_html_uses_display_none(self):
        data = generate.prompt_inject_hidden_html_md()
        self.assertIn(b"display:none", data)
        self.assertIn(b"<span", data)

    def test_unicode_tags_payload_uses_tag_block(self):
        data = generate.prompt_inject_unicode_tags_md().decode("utf-8")
        # Tag-block codepoints (U+E0000..U+E007F) are invisible but present.
        tag_chars = [c for c in data if 0xE0000 <= ord(c) <= 0xE007F]
        self.assertGreater(len(tag_chars), 30,
                           "expected the encoded injection to contribute "
                           "many tag-block codepoints")

    def test_exfil_md_has_external_image_link(self):
        data = generate.prompt_inject_exfil_md()
        self.assertIn(b"https://attacker.example.com/", data)
        self.assertIn(b"![", data, "must be a markdown image link")


# ---------------------------------------------------------------------------
# Fetcher behaviour without network access (filter + manifest parsing only)
# ---------------------------------------------------------------------------


class FetcherFilter(unittest.TestCase):
    def setUp(self):
        self.entries = fetch._load_manifest(SEED_MANIFEST)

    def test_filter_by_type(self):
        out = fetch._filter_entries(self.entries, {"pdf"}, None, None, None)
        self.assertTrue(out, "no PDFs in seed manifest")
        for e in out:
            self.assertEqual(e.file_type, "pdf")

    def test_filter_by_company(self):
        out = fetch._filter_entries(self.entries, None, {"beta"}, None, None)
        self.assertTrue(out, "no Beta entries in seed manifest")
        for e in out:
            self.assertEqual(e.company_tag, "beta")

    def test_filter_by_license(self):
        out = fetch._filter_entries(
            self.entries, None, None, {"us-gov-public-domain"}, None)
        self.assertTrue(out)
        for e in out:
            self.assertEqual(e.license, "us-gov-public-domain")

    def test_limit_caps_result_size(self):
        out = fetch._filter_entries(self.entries, None, None, None, 2)
        self.assertEqual(len(out), 2)

    def test_zero_limit_returns_empty(self):
        out = fetch._filter_entries(self.entries, None, None, None, 0)
        self.assertEqual(out, [])

    def test_filter_combinations(self):
        out = fetch._filter_entries(
            self.entries, {"pdf"}, {"beta"}, None, None)
        for e in out:
            self.assertEqual(e.file_type, "pdf")
            self.assertEqual(e.company_tag, "beta")

    def test_dest_subdir_uses_tenant_layout(self):
        e = self.entries[0]
        self.assertEqual(e.dest_subdir,
                         f"{e.company_tag}/{e.org_unit_slug}")

    def test_manifest_entry_rejects_unknown_file_type(self):
        bad = {
            "url": "https://example.com/x",
            "filename": "x",
            "file_type": "exe",  # not allowed
            "license": "public-domain",
            "source": "test",
            "company_tag": "acme",
            "org_unit_slug": "engineering",
        }
        with self.assertRaises(ValueError):
            fetch.ManifestEntry.from_dict(bad)

    def test_manifest_entry_rejects_missing_required_key(self):
        bad = {
            "url": "https://example.com/x",
            # 'filename' missing
            "file_type": "pdf",
            "license": "public-domain",
            "source": "test",
            "company_tag": "acme",
            "org_unit_slug": "engineering",
        }
        with self.assertRaises(ValueError):
            fetch.ManifestEntry.from_dict(bad)


# ---------------------------------------------------------------------------
# Rate limiter (zero network calls)
# ---------------------------------------------------------------------------


class RateLimiter(unittest.TestCase):
    def test_per_host_rate_is_applied(self):
        # 4 req/s = 250ms between requests. Two back-to-back calls should
        # cause the second to sleep at least ~200ms (with some tolerance).
        import time
        rl = fetch.HostRateLimiter({"_default_": 4.0})
        rl.wait("first.example")  # primes the bucket
        t0 = time.monotonic()
        rl.wait("first.example")
        elapsed = time.monotonic() - t0
        self.assertGreaterEqual(elapsed, 0.20,
                                f"rate limiter slept only {elapsed:.3f}s; "
                                f"expected >=0.20s for 4 req/s")

    def test_unknown_host_uses_default(self):
        rl = fetch.HostRateLimiter({"_default_": 1000.0})  # ~1 ms gap
        rl.wait("unknown.example")  # should not raise


if __name__ == "__main__":
    unittest.main(verbosity=2)
