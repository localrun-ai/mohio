# Wikore Iteration 2 - Access Resolution and Retrieval (Design)

**Status:** draft, pre-implementation.
**Audience:** Opus 4.8 (the reviewer of this document) and future-me.
**Scope:** the security boundary and the read path. The most important
iteration in the project; the one where a single subtle bug becomes a
data leak.

This document fixes the design before code lands. Section 1 restates
what `docs/development_plan.md` already commits to for Iter 2 so the
review has a baseline. Section 2 adds five concrete refinements that
came out of an r/RAG design-review thread; they tighten revocation
correctness and stale-cache detectability. Section 3 is the open
question to challenge.

---

## 1. Baseline: what Iter 2 already commits to

From `docs/development_plan.md` Iteration 2:

1. **`AccessResolver`** port + Postgres adapter. Resolves
   `(Principal, Tenant)` to an `AccessScope` (the set of `org_unit_id`s
   the principal can read), respecting `principal_applies_to`
   (`self_only` / `self_and_descendants`) and membership expiry.

2. **Redis cache (`lr:eff:*`)** keyed per `(company_id, user_id)`. Per
   V008, every entry carries the `access_epoch` it was computed under;
   if the company-level epoch has moved on, the entry is discarded.
   The epoch path is the break-glass / emergency-revoke channel.

3. **`QdrantFilterBuilder`** translates `(AccessScope, Principal,
   sensitivity policy)` into a `QdrantFilter`:
   `MatchValue(company_id) AND MatchAny(access_scope_ids)
   AND MatchAny(sensitivity_labels) AND MatchValue(lifecycle_status='active')`.

   (PR #24 already added `sensitivity_labels` as a required filter
   field with fail-closed semantics on empty sets. The `QdrantFilter`
   struct is implemented; the builder is not.)

4. **`VectorRetriever`** adapter (Qdrant HTTP) returns ranked
   `ChunkCandidate`s with payload attached.

5. **EvidenceGate** as a *type contract*: only `AllowedCandidate`
   values can travel to the reranker. The conversion
   `ChunkCandidate -> AllowedCandidate` runs the Postgres
   re-validation (lifecycle, sensitivity, scope membership, tenant);
   anything that fails to convert is dropped.

6. **Postgres hydration** turns surviving payloads into typed
   `AllowedCandidate`s with chunk text fetched from PG. Invariant:
   **Qdrant is the index, Postgres is the evidence.**

7. **As-of retrieval**: query parameter clamps to
   `activated_at <= as_of AND (superseded_at IS NULL OR superseded_at > as_of)`.

8. **Section expansion (K5)**: fetch parent and sibling chunks by
   `section_id`, route them through EvidenceGate before joining the
   result set.

This is correct as far as it goes. The gap is in what happens *between*
a grant change and the Qdrant payload catching up.

---

## 2. Refinements that should land in Iter 2

These five additions tighten the design without changing its shape.
The common thread: **stale payloads should be detectable, not merely
unlikely.**

### 2.1 Per-chunk ACL version + tenant ACL epoch

**Why.** The current epoch lives in Redis at company scope; bumping it
forces every user's resolved scope cache to recompute, but the *Qdrant
payloads themselves* carry no version, so a stale chunk that passed
the Qdrant prefilter has no way to be recognised as stale by the
EvidenceGate without a full PG re-resolve of its `access_scope_ids`.

**Add.**

- Schema:
  ```sql
  -- V0xx in iter-2's migration window. acl_version is a column on documents,
  -- not a separate table: it is 1:1 with the document and churns at admin
  -- rate (per-minute at worst, per V024), so the write/vacuum-locality
  -- argument for a side table does not apply. qdrant_synced_version is the
  -- monotonic "latest version actually written to Qdrant" (see 2.3).
  ALTER TABLE companies
      ADD COLUMN acl_epoch BIGINT NOT NULL DEFAULT 1;

  ALTER TABLE documents
      ADD COLUMN acl_version           BIGINT NOT NULL DEFAULT 1,
      ADD COLUMN qdrant_synced_version BIGINT NOT NULL DEFAULT 0;
  ```

- Bump `companies.acl_epoch` in the same transaction as any grant or
  org-tree mutation that could change effective visibility for any
  user (revoke, move_org_unit, etc).

- Bump `documents.acl_version` in the same transaction as any
  grant whose `resource_id` is this document (and as a fan-out from
  org-unit grants that intersect the document's owner subtree).

- `ChunkPayload` (bump `kSchemaVersion` to 3):
  ```cpp
  std::int64_t acl_epoch     = 0;  // companies.acl_epoch at write time
  std::int64_t acl_version   = 0;  // documents.acl_version at write time
  ```

- EvidenceGate, in addition to its existing re-resolve:
  - Reject if `payload.acl_epoch < tenant_current_epoch` AND
    `payload.acl_version < doc_current_acl_version`.
  - The epoch check is the cheap fast-path (single Redis GET per
    tenant per request).
  - The doc version check is the precise one (single PG row per
    candidate document; batched by document_id).

**Why both.** Tenant epoch is one int, cheap to compare; it catches
the "subtree moved, every doc maybe affected" case without needing
per-doc state. The per-document version catches the targeted case
("this one document's grants changed") without forcing every read in
the tenant to fall through to PG.

**Relationship to existing mechanisms (V008 / V015).** `companies.acl_epoch`
is the authoritative company-wide scope-invalidation signal; the V008
`lr:eff:keys:company:{C}` reverse-index Sets are demoted to a best-effort
eager-evict optimization (crash-safe correctness lives in the monotonic epoch
compare, not in a DEL that can be lost). The per-document rewrite jobs ride the
existing V015 outbox - idempotent, coalesced by its UNIQUE
`(company_id, job_type, idempotency_key)`, ordered by `created_at` - carrying
`acl_version` in the JSONB payload. Do not stand up a second queue alongside
V008's `lr:resync:q`.

### 2.2 Revocation overlay (Redis deny-list)

**Why.** Grants can lag; revokes shouldn't. Bumping the tenant epoch
invalidates resolver caches but does not delete already-embedded
chunks from Qdrant, so during the outbox-driven payload rewrite
window (seconds to minutes for a large subtree move) a chunk whose
ACL set just shrunk is still served by the Qdrant prefilter.

**Add.**

- Redis Set: `lr:acl:deny:{company_id}` containing `document_id` strings
  (per-document, not per-chunk). A 10k-document subtree move adds 10k members,
  not hundreds of thousands of chunk ids. The EvidenceGate already knows each
  candidate's document via the payload, so per-document granularity is
  sufficient; the V008 model never revokes a single chunk independently of its
  document.
  Populated synchronously when a grant is revoked, an org-unit is
  moved, or a document is reassigned.
- Drained by the same outbox worker that rewrites the payloads: once the
  document's chunks are re-upserted and `documents.qdrant_synced_version`
  has caught up to the bumped `acl_version`, the `document_id` is `SREM`-ed
  from the deny-list.
- EvidenceGate checks `lr:acl:deny:{company_id}` membership for each
  candidate's document before the PG re-validation. Membership = reject
  without further work.

**Cost.** One pipelined `SMISMEMBER` per query over the candidates' distinct
document ids (far fewer than the ~50 chunk candidates, since candidates cluster
by document). Memory: bounded by the count of documents with an in-flight
revoke between outbox catch-up windows; for normal operation this is small.

**Fail-open to the PG gate.** If the deny-list Redis is unreachable, the
EvidenceGate skips the deny check and lets every candidate fall through to the
authoritative Postgres re-validation (baseline item 5). That is slower (more
candidates hit PG) but fully available and still correct: the deny-list is a
fast-reject optimization, not the security boundary, so losing it degrades
recall latency, never isolation. Rejecting the whole tenant would turn a Redis
blip into a tenant-wide search outage for no correctness gain.

### 2.3 Monotonic ACL version on outbox payload-rewrite jobs

**Why.** The outbox worker is multi-instance and may process events
out of order under retry or partition. If an older payload-rewrite
job (carrying `acl_version = 3`) lands after a newer one (`acl_version
= 4`), it would silently downgrade the payload to a stale ACL set.

**Add.**

- Every payload-rewrite outbox event (a V015 `outbox_events` row with
  `job_type = 'qdrant_resync_chunk_acl'`, not a new queue) carries the
  `acl_version` it was created at (snapshot at outbox-write time).
- The worker reads the current `documents.acl_version` for
  the affected document and:
  - If `event.acl_version < current.acl_version`, drop the event as
    superseded (log INFO, increment a counter).
  - Else, compute the new payload and upsert with `acl_version =
    current.acl_version` (re-read at write time, not the event time).

### 2.4 Stale-candidate metric

**Why.** Cache rot is the failure mode this iteration is trying to
prevent. The system should make it observable in a single number, not
require log diving.

**Add.**

```cpp
// Counter incremented inside EvidenceGate's rejection branch.
wikore_retrieval_pg_revalidation_failed_total{
    reason = "acl" | "lifecycle" | "sensitivity" | "deny_overlay" | "tenant_mismatch"
}
```

A non-zero baseline is expected (lifecycle transitions, deliberate
revocations); a *rising* baseline relative to total queries indicates
either the outbox is lagging, the deny-list is growing, or the
payload rewrites are not draining. Alert on the rate, not the
absolute count.

### 2.5 Thundering-herd shape for subtree operations

**Why.** A `move_org_unit` operation on a subtree of 10k documents
synchronously rewrites 10k Qdrant payloads if done naively. That is
the wrong correctness ordering: it leaves the system inconsistent for
the duration, and the per-payload write is serialised against the
embed pipeline.

**Pattern.**

1. **In the same PG transaction as the move:**
   - Bump `companies.acl_epoch`.
   - For each affected document: bump `documents.acl_version` and
     `SADD lr:acl:deny:{tenant} document_id` (via outbox so the Redis write is
     atomic with the PG change). Per-document, so the set grows by the document
     count, not the chunk count.
2. **Synchronously after commit:** correctness is already restored.
   Every reader's EvidenceGate either bounces on the epoch (cheap),
   on the doc version (per-doc cheap), or on the deny-list (per-chunk
   cheap). PG re-validation handles edge cases.
3. **Asynchronously, in bounded batches:** outbox worker rewrites
   chunk payloads with the new `acl_version`; when a document's
   `qdrant_synced_version` reaches its `acl_version`, the `document_id` is
   `SREM`-ed from the deny-list. Search performance degrades smoothly
   (more candidates fall through to PG re-validation) until the
   rewrite drains.

**Invariant.** At no point is a chunk simultaneously:
- not in the deny-list,
- carrying a `payload.acl_version >= current_acl_version`,
- and granted to the requester by the *old* ACL but not the new.

This is the property worth pinning in a property test (in addition to
the existing Iter 2 leakage tests).

---

## 3. Open question for Opus

This is the part I want challenged. The previous five sections
describe a design; this section asks whether it composes.

**Setup.**

Three signals gate retrieval, in order of evaluation:

1. `companies.acl_epoch` (tenant-wide, fast-path).
2. `documents.acl_version` (per-document, cached briefly).
3. `lr:acl:deny:{tenant}` (per-document, Redis set).

Outside the read path, three writers update them:

- **W1**: a grant change (revoke or new grant on a specific document)
  bumps `(epoch, doc_version)` for that document only, and enqueues
  payload rewrites for that document's chunks.
- **W2**: an org-unit `move_org_unit` operation rotates a subtree of
  N documents; bumps `epoch`, every affected `doc_version`, and
  enqueues N×chunks rewrites.
- **W3**: the outbox worker drains payload rewrites; on success, upserts the
  chunk with the new `acl_version`, and once the document's
  `qdrant_synced_version` reaches its `acl_version`, `SREM`s the `document_id`
  from the deny-list.

**The question to challenge.**

Is the following invariant preserved under all interleavings of W1,
W2, and W3?

> For any reader R observing the tenant at wall-clock time t,
> R cannot receive a chunk c such that:
>
> - c.access_scope_ids (as resolved at t from authoritative Postgres)
>   does not intersect R's effective scope at t,
>
> regardless of whether the Qdrant payload for c has been rewritten
> yet, and regardless of whether the deny-list contains c.

In particular, I want concrete attack scenarios for these
interleavings:

- **A. Move-then-revoke race.** W2 in flight for subtree S. W1 begins
  while W2's outbox draining is partway done. W1's epoch bump is a
  no-op (W2 already bumped it). W1's deny-list write SADDs the
  revoked chunks. W2's worker, holding a snapshot of the
  pre-W1-revoke ACL, rewrites a payload with the *new* `acl_version`
  but the *old* (still-permitted-by-W2) `access_scope_ids`, and
  `SREM`s the deny-list entry W1 just placed. Is this a leak? If yes,
  what is the smallest fix?

- **B. Out-of-order outbox.** W3 has two events for the same chunk c:
  e1 (`acl_version=4`) and e2 (`acl_version=5`). e2 is processed
  first. e1 arrives after a worker restart. The §2.3 guard
  (`event.acl_version < current.acl_version => drop`) handles this,
  but: does the guard need to be inside the Qdrant upsert
  transaction, or can it be checked-then-upserted in two steps? If
  two steps, what prevents another writer racing in between?

- **C. Deny-list drained too early.** W3 commits the Qdrant upsert
  successfully. Before W3's `SREM`, R reads c. R sees the new payload
  (correct ACL) but the deny-list still contains c, so EvidenceGate
  rejects a correctly-granted chunk. Acceptable false-negative
  (search returns less than the user could see) or wrong?

- **D. Deny-list drained too late.** Inverse: Qdrant upsert
  succeeds, W3 crashes before `SREM`. The chunk is in the deny-list
  forever (until a sweeper notices). What's the sweep predicate?
  "Chunk in deny-list where payload.acl_version >= current_acl_version
  for all access_scope_ids it carries" is plausible but expensive.

- **E. Reader's resolved scope is stale.** R's `lr:eff:R` entry was
  computed at epoch=N. W1 bumps epoch to N+1. R's next request:
  the cache is discarded, the resolver re-computes, but the resolver
  itself is a transaction over `memberships` and `resource_grants`;
  what isolation level keeps W1's revocation visible to R's
  re-resolution? Read Committed is the obvious answer, but is there
  a phantom-read risk on the closure rebuild during a concurrent
  org-unit move?

**What I want from the review.**

For each of A-E, the smallest correctness fix to the design (or a
yes / no with justification if the design as written is already
safe). For A specifically, I want a concrete sequence diagram or a
"this cannot happen because X" with X named.

If there is a sixth scenario that is more important than A-E and I
missed it, please surface it.

---

## 4. Things deliberately deferred to Iter 3

- The actual reranker (`authority_level`-weighted; `AllowedCandidate`
  in, ranked `AllowedCandidate` out). Iter 2 produces the candidate
  set; ranking inside it is iter 3.
- The chat endpoint and LLM streaming. Iter 2 is read-path
  primitives only.
- The resync worker for org-tree moves at very large scale (>100k
  documents per move). The pattern in §2.5 works at the 10k scale
  mentioned in the original challenge; >100k may need a different
  shape (e.g. partition the deny-list, batch outbox enqueue).
- Cross-region replication. Out of scope for self-hosted single-region
  deployments; revisit if multi-region becomes a requirement.

---

## 5. Acceptance for this design before Iter 2 coding starts

1. Opus 4.8 review of Section 3 returns either "design holds" or a
   list of concrete fixes for the named scenarios.
2. Section 2 schema changes are written into a V0xx migration draft
   alongside this document (not in this document; it stays a design
   note).
3. The property test in §2.5's invariant is sketched out in
   `tests/test_retrieval_invariants.cpp` as a `[wip]` test so the
   shape is committed before the implementation lands.

Once those three land, Iter 2 implementation can begin.
