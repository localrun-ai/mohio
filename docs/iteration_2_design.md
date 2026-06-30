# Wikore Iteration 2 - Access Resolution and Retrieval (Design v2.3)

**Status:** post-Opus-review revision (round 4); pre-implementation.
**Audience:** future-me and any reviewer who needs to know what is
load-bearing and what is not.
**Predecessor:** `iteration_2_design_v1.md` (the original draft) and
`iteration_2_design_feedback_opus.md` (the first review). v2.0
integrated the first round; v2.1 fixed §2.6 after Opus's second
round (per-user `scope_epoch`); v2.2 fixed §2.6 again after Opus's
third round corrected a factual mistake about the auth path (it is
itself cached, so `principal.scope_epoch` is at the identity-cache
ceiling, not request-time authoritative). v2.3 closes the §0
resource-axis OPEN item: G1 adopts option (a) (live resource-axis
resolution via a reader-side-inversion query); `dc.access_scope_ids`
is demoted to a Qdrant prefilter hint and renamed; option (c)
(write-trigger tombstone) was considered and dismissed.

This document fixes the design before code lands. Section 0 names the
two correctness invariants that everything else either supports or is
optional with respect to. Section 1 restates the eight Iter-2
deliverables `docs/development_plan.md` already commits to, with the
small clarification that as-of reads bypass the live overlay. Section
2 describes the optional optimizations (per-doc ACL version, deny-list
overlay, reader-axis epoch, schema-v3 backfill) and shows for each how
its failure mode is bounded by G1. Section 3 walks the six interleaving
scenarios with their now-known resolutions. Section 5 spells out the
acceptance criteria expressed against G1, not against the overlay's
internal invariant.

---

## 0. Invariants (load-bearing)

Everything in §2 is optional with respect to these two. Anything that
contradicts them is a bug, regardless of how clever the overlay is.

### G1 - Gate authority

**No `ChunkCandidate` becomes an `AllowedCandidate` except by passing
a single authoritative Postgres query, evaluated under one snapshot,
that re-checks:**

- tenant match (`company_id`),
- lifecycle (`= 'active'` for live reads, V024-clamped for as-of),
- sensitivity policy (the reader's allowed-label set contains
  `payload.sensitivity_label`),
- and **resource-axis visibility resolved live** - the chunk's document
  is currently granted to some org_unit in `reader_scope`, computed from
  `documents.owner_org_unit_id` + `resource_grants` + `org_unit_closure`
  at read time (option (a); see the query below). `reader_scope` itself
  is the reader's authoritatively-resolved scope, **not** their cached
  scope (see §2.6 for the reader-axis `scope_epoch` mechanism).
  `dc.access_scope_ids` (renamed `dc.qdrant_prefilter_scope_ids` in V032)
  is a denormalized prefilter hint and is **not** consulted by the gate.

**The Qdrant payload is an input to candidate selection only, never to
authorization.** All overlay state (epoch, version, deny-list) is a
hint that influences which candidates reach the gate; nothing in the
overlay can make a stale-payload chunk legitimately survive the gate.

Two properties make G1 cheap enough to actually be the boundary:

1. **One query, not N - resolved on the reader side.** The expensive way
   to resolve the resource axis live is the ingest-style 5-arm UNION that
   expands every grant's principal subtree per document
   (`fetch_access_scopes` in `document_repo.cpp`). Run per-candidate at
   read time, that re-expands large subtrees ~30x per query and is the
   regression to avoid. Instead, **invert the expansion to the reader
   side, once per request**; then the per-document test is three cheap
   EXISTS arms.

   Step A (once per request) - the set of org_units whose grant would
   reach this reader (self_only via `reader_scope`, self_and_descendants
   via an ancestor):
   ```sql
   WITH reader_grant_keys AS (
       SELECT unnest($4::uuid[]) AS ou_id          -- reader_scope
       UNION
       SELECT c.ancestor_id                        -- ancestors of reader_scope
       FROM   org_unit_closure c
       WHERE  c.company_id    = $1
         AND  c.descendant_id = ANY($4::uuid[])
   )
   ```
   Size is bounded by `|reader_scope| x tree_depth` (low thousands worst
   case), computed with one indexed closure scan.

   Step B (the gate, ~50 candidates / ~10-30 distinct documents):
   ```sql
   SELECT dc.id, dc.section_id, dv.sensitivity_label,
          dv.lifecycle_status, dv.activated_at, dv.superseded_at,
          d.authority_level, d.owner_org_unit_id
   FROM   document_chunks   dc
   JOIN   document_versions dv ON dv.id = dc.document_version_id
   JOIN   documents         d  ON d.id  = dv.document_id
   WHERE  dc.company_id        = $1
     AND  dv.lifecycle_status  = ANY($2)
     AND  dv.sensitivity_label = ANY($3)
     AND  dc.id                = ANY($5::uuid[])
     AND  (
         -- arm 1: reader is in the document's owner scope (short-circuits
         --        the common broad-scope reader).
         d.owner_org_unit_id = ANY($4::uuid[])
         -- arm 2: a document-level read grant whose principal reaches reader.
      OR EXISTS (
             SELECT 1 FROM resource_grants rg
             WHERE  rg.company_id    = $1
               AND  rg.resource_type = 'document'
               AND  rg.resource_id   = d.id
               AND  rg.permission IN ('read','write','admin')
               AND  (rg.expires_at IS NULL OR rg.expires_at > now())
               AND  rg.principal_id IN (SELECT ou_id FROM reader_grant_keys)
         )
         -- arm 3: an org-unit read grant on an ancestor of the owner whose
         --        principal reaches reader (resource subtree via closure).
      OR EXISTS (
             SELECT 1
             FROM   resource_grants  rg
             JOIN   org_unit_closure rc
                 ON rc.company_id    = $1
                AND rc.ancestor_id   = rg.resource_id
                AND rc.descendant_id = d.owner_org_unit_id
             WHERE  rg.company_id    = $1
               AND  rg.resource_type = 'org_unit'
               AND  rg.permission IN ('read','write','admin')
               AND  (rg.expires_at IS NULL OR rg.expires_at > now())
               AND  (rg.resource_applies_to = 'self_and_descendants'
                     OR (rg.resource_applies_to = 'self_only' AND rc.depth = 0))
               AND  rg.principal_id IN (SELECT ou_id FROM reader_grant_keys)
         )
     )
   ```
   **The arms above are illustrative of the LOGIC, not the production query
   shape.** Written as a correlated EXISTS per candidate row, Postgres
   re-scans `resource_grants` once per candidate and the all-reject worst
   case is 3-5x over the 10ms budget (benchmarked - §5 item 5). The
   production gate MUST resolve the **distinct candidate documents in a
   single set-based pass** so the grant table is scanned once via hash
   joins:

   ```sql
   WITH reader_grant_keys AS ( ... as Step A ... ),
   cand_docs AS (                       -- distinct docs behind the ~50 chunks
       SELECT DISTINCT d.id AS doc_id, d.owner_org_unit_id AS owner
       FROM document_chunks dc
       JOIN document_versions dv ON dv.id = dc.document_version_id
       JOIN documents d ON d.id = dv.document_id
       WHERE dc.company_id = $1 AND dc.id = ANY($5::uuid[])
         AND dv.lifecycle_status = ANY($2) AND dv.sensitivity_label = ANY($3)
   ),
   visible AS (
       SELECT doc_id FROM cand_docs WHERE owner = ANY($4::uuid[])      -- arm 1
     UNION
       SELECT cd.doc_id FROM cand_docs cd                              -- arm 2
       JOIN resource_grants rg ON rg.company_id=$1 AND rg.resource_type='document'
            AND rg.resource_id=cd.doc_id AND rg.permission IN ('read','write','admin')
            AND (rg.expires_at IS NULL OR rg.expires_at>now())
       WHERE (rg.principal_applies_to='self_only'
              AND rg.principal_id = ANY($4::uuid[]))            -- reader_scope
          OR (rg.principal_applies_to='self_and_descendants'
              AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys))
     UNION
       SELECT cd.doc_id FROM cand_docs cd                              -- arm 3
       JOIN org_unit_closure rc ON rc.company_id=$1 AND rc.descendant_id=cd.owner
       JOIN resource_grants rg ON rg.company_id=$1 AND rg.resource_type='org_unit'
            AND rg.resource_id=rc.ancestor_id AND rg.permission IN ('read','write','admin')
            AND (rg.expires_at IS NULL OR rg.expires_at>now())
            AND (rg.resource_applies_to='self_and_descendants'
                 OR (rg.resource_applies_to='self_only' AND rc.depth=0))
       WHERE (rg.principal_applies_to='self_only'
              AND rg.principal_id = ANY($4::uuid[]))            -- reader_scope
          OR (rg.principal_applies_to='self_and_descendants'
              AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys))
   )
   SELECT dc.id, dc.section_id, dv.sensitivity_label, dv.lifecycle_status,
          d.authority_level, d.owner_org_unit_id
   FROM document_chunks dc
   JOIN document_versions dv ON dv.id = dc.document_version_id
   JOIN documents d ON d.id = dv.document_id
   WHERE dc.company_id = $1 AND dc.id = ANY($5::uuid[])
     AND dv.lifecycle_status = ANY($2) AND dv.sensitivity_label = ANY($3)
     AND d.id IN (SELECT doc_id FROM visible);
   ```

   Measured ~5-8ms on the worst-case tenant for both all-visible and
   all-reject (§5 item 5). `dc.qdrant_prefilter_scope_ids` is not
   referenced; resource visibility is computed from live grants. The
   overlay's role is to keep `$5` short (fewer candidate docs to resolve),
   not to replace this query.

   **`principal_applies_to` matters (arms 2 and 3).** A `self_only`-principal
   grant applies only to its exact principal OU, so it matches iff the
   principal is in `reader_scope` (`$4`). A `self_and_descendants`-principal
   grant matches iff the principal is in `reader_grant_keys` (reader_scope +
   ancestors). Matching every grant against `reader_grant_keys` - the first
   cut of this query - over-grants a `self_only` grant to a reader sitting in
   a descendant of the principal (an over-grant = leak). The §5-item-1
   property test (`tests/test_retrieval_invariants.cpp`) catches exactly this
   by checking the gate against an independent oracle
   (`fetch_access_scopes` resource-side expansion intersected with
   `effective_read_orgs`).

2. **One snapshot.** The query reads `document_chunks`, `document_versions`,
   `documents`, `resource_grants`, and `org_unit_closure` in one statement
   under Read Committed, so there is no torn-state window: the resource-axis
   resolution sees the same committed snapshot as everything else, and a
   grant revoke committed before the read is visible (this is what makes
   scenario A safe; see §3.A).

   **RESOLVED (v2.3) - option (a), live resource-axis resolution.**
   `dc.access_scope_ids` was a *denormalized* column (V003) recomputed only
   by the async resync worker (V002 comment: "resync worker hot path ...
   recompute access_scope_ids"; V008 routes grant create/revoke through
   `lr:resync:q`), with no synchronous trigger. A gate that filtered the
   resource axis on that column would be exactly as stale as the Qdrant
   payload between a revoke and the resync, and would leak. v2.3 resolves
   this by **option (a)**: the gate resolves the resource axis live from
   `resource_grants` + `org_unit_closure` (the query above), so PG is
   genuinely the evidence and the deny-list (§2.2) can fail-open. The
   column is demoted to a Qdrant prefilter hint and renamed
   `dc.qdrant_prefilter_scope_ids` (V032) so its non-authority status is
   obvious on disk. It remains load-bearing for the **write** path only
   (the Qdrant payload source in `outbox_worker.cpp` and the
   resync-targeting GIN index), never for the gate.

   Option (b) (synchronously rewrite the column in the grant transaction)
   and option (c) (a per-chunk `dirty` tombstone) were both considered;
   (c) is dismissed in the changelog. (a) is bounded at enterprise scale
   provided the query is written as the reader-side inversion above and
   not the ingest-style per-document expansion; the cost bound holds for
   the MVP `principal_type='org_unit'` grant model and must be re-derived
   when user/group grants (the access_tokens model) land - see §4 and
   §5 item 5.

### G2 - Same-transaction monotonic bump

**Every `companies.acl_epoch` and `documents.acl_version` bump is in
the same Postgres transaction as the authoritative change it describes**
(the membership/grant/move row mutation, etc.).

Consequences:

- A reader that observes epoch `N+1` is reading a snapshot in which
  the authoritative change is also committed, so the re-resolve sees
  the change. Causality holds for free under Read Committed; no extra
  locking required.
- A worker that reads `documents.acl_version = v` and recomputes the
  payload is reading the same snapshot as the grant rows; the payload
  it computes is consistent with that `v`.
- If a writer can bump the epoch *without* the authoritative change
  being durable, G2 is violated and G1 may be defeated. The check is:
  every code path that bumps `acl_epoch` or `acl_version` must be
  inside the same `BEGIN ... COMMIT` block as the row mutation,
  ideally enforced by a SQL function (e.g.
  `revoke_resource_grant(...)`) that does both.

### What G1+G2 buy us

Under G1 and G2:

- Any stale overlay state is a **recall** problem (false negatives at
  the gate or unnecessary fall-through to the gate), never a **leak**
  problem (false positives past the gate).
- Failure modes for the overlay become: "Redis is down" -> "PG load
  rises", not "tenant cannot search" and never "leak".
- The whole of §2 can therefore be sized for *performance budget*, not
  for *security guarantee*. The two concerns are decoupled.

---

## 1. Baseline: what Iter 2 already commits to

From `docs/development_plan.md` Iteration 2:

1. **`AccessResolver`** port + Postgres adapter. Resolves
   `(Principal, Tenant)` to an `AccessScope` (the set of `org_unit_id`s
   the principal can read), respecting `principal_applies_to`
   (`self_only` / `self_and_descendants`) and membership expiry.
   The resolver MUST execute as a single SQL statement (see §2.6 / E)
   so its output is one Read Committed snapshot and cannot tear under
   a concurrent `move_org_unit`.

2. **Redis cache (`lr:eff:*`)** keyed per `(company_id, user_id)`. Per
   V008, every entry carries the `access_epoch` it was computed under;
   if the company-level epoch has moved on, the entry is discarded.
   The epoch path is the break-glass / emergency-revoke channel
   (resource axis only); membership / group-member changes are handled
   per-user via §2.6's `scope_epoch`, not the company epoch.

3. **`QdrantFilterBuilder`** translates `(AccessScope, Principal,
   sensitivity policy)` into a `QdrantFilter`:
   `MatchValue(company_id) AND MatchAny(access_scope_ids)
   AND MatchAny(sensitivity_labels) AND MatchValue(lifecycle_status='active')`.

   (PR #24 already added `sensitivity_labels` as a required filter
   field with fail-closed semantics on empty sets. The `QdrantFilter`
   struct is implemented; the builder is not.)

   Note (v2.3): the Qdrant prefilter still matches on the resolved scope
   set in the payload, **not** on `owner_org_unit_id` alone. Owner-only
   would miss grant-only visibility (a reader who sees a chunk solely via
   a `resource_grant`) and produce false negatives at the index. Only the
   Postgres column is renamed (`qdrant_prefilter_scope_ids`, V032); the
   Qdrant payload field keeps its name. Under (a) the prefilter's staleness
   is a pure recall concern - an over-stale prefilter just lets candidates
   fall through to the gate, which is now the authority - so matching on
   the resolved set is correct and safe.

4. **`VectorRetriever`** adapter (Qdrant HTTP) returns ranked
   `ChunkCandidate`s with payload attached.

5. **EvidenceGate** as a *type contract*: only `AllowedCandidate`
   values can travel to the reranker. The conversion
   `ChunkCandidate -> AllowedCandidate` runs the G1 query against PG;
   anything that fails to convert is dropped. The §2 overlay state
   feeds the *pre-gate filtering* (decides which candidates reach the
   gate query at all), not the gate's verdict.

6. **Postgres hydration** turns surviving payloads into typed
   `AllowedCandidate`s with chunk text fetched from PG. Invariant:
   **Qdrant is the index, Postgres is the evidence.** G1 is the
   mechanical statement of that invariant.

7. **As-of retrieval**: the query parameter clamps to
   `activated_at <= as_of AND (superseded_at IS NULL OR superseded_at > as_of)`.
   **As-of reads bypass the live overlay entirely.** They authorize
   against historical state via V024's `*_history` tables; consulting
   the live `acl_epoch`, `acl_version`, or `lr:acl:deny:*` would
   either wrongly deny a historical read (false negative) or, worse,
   wrongly admit one if the overlay were ever treated as authoritative.
   As-of's gate query reads `resource_grants_history` and
   `memberships_history` valid-at-`as_of` rows; the live tables and the
   live overlay are untouched on this path.

8. **Section expansion (K5)**: fetch parent and sibling chunks by
   `section_id`, route them through EvidenceGate before joining the
   result set.

---

## 2. Optimizations under G1

These are recall and performance optimizations that keep the gate
query cheap by reducing the candidate set before it gets there. None
is on the correctness path; G1 is. Each subsection states what failure
looks like and why it is bounded.

### 2.1 Per-document ACL version + tenant ACL epoch

**Why.** The live resource-axis resolve (§0 arm 2/arm 3) is cheap when
the candidate set is small (~50) and `reader_grant_keys` is small, and
grows with both: a poorly-pre-filtered candidate set runs the EXISTS
arms for more documents, and a reader with thousands of OUs grows
`reader_grant_keys`. The overlay does not let any candidate skip the
resolve (G1 is unconditional); it keeps the candidate set (`$5`) small
so the gate runs the grant EXISTS arms for fewer documents. A candidate
whose payload epoch + version are stale, or whose document is denied, is
dropped pre-gate rather than resolved.

**Schema.**

```sql
-- V032 (iter-2's migration window). acl_version is a column on documents,
-- not a separate table: it is 1:1 with the document and churns at admin
-- rate (per-minute at worst, per V024), so the write/vacuum-locality
-- argument for a side table does not apply. qdrant_synced_version is the
-- monotonic "latest version actually written to Qdrant" used by §2.3.
ALTER TABLE companies
    ADD COLUMN acl_epoch BIGINT NOT NULL DEFAULT 1;

ALTER TABLE documents
    ADD COLUMN acl_version           BIGINT NOT NULL DEFAULT 1,
    ADD COLUMN qdrant_synced_version BIGINT NOT NULL DEFAULT 0;

-- Demote the denormalized chunk scope to an explicit Qdrant prefilter hint.
-- Under G1/(a) the gate resolves the resource axis live from resource_grants;
-- this column is only the Qdrant payload source (outbox_worker.cpp) and the
-- resync-targeting GIN index, never the access boundary. Renaming makes the
-- non-authority status obvious so no future reader mistakes it for the gate.
ALTER TABLE document_chunks
    RENAME COLUMN access_scope_ids TO qdrant_prefilter_scope_ids;
ALTER INDEX document_chunks_scopes_idx
    RENAME TO document_chunks_prefilter_scope_idx;
```

**Bump points (all per G2 - same-tx as the authoritative change).**

The two signals gate two different caches, so they bump on two different
classes of event. Mixing them is the v2.2-era mistake corrected here (and
implemented in V032):

- `companies.acl_epoch` gates the `lr:eff` **reader-scope** cache (Section 1
  item 2). A reader's scope is their org_unit memberships expanded by the
  closure; it changes only when the org TREE STRUCTURE shifts for many users
  at once. Bump it on:
  - `org_units` create / delete / `move_org_unit` (V012) - the closure, and
    therefore the resolved scope of `self_and_descendants` members, changes.
  - Explicit break-glass: a SQL function the operator calls to invalidate
    every `lr:eff` cache in the tenant immediately (mass-incident response,
    suspected compromise).

  It does **not** bump on `resource_grants` or `owner_org_unit_id` changes:
  those are RESOURCE-axis events that never alter who a user is, so they must
  not invalidate every tenant `lr:eff` (the same tenant-wide-stampede
  pathology §2.6 removed on the membership side). Membership / group-member
  changes likewise do not bump the company epoch; they bump the per-user
  `users.scope_epoch` (§2.6).

- `documents.acl_version` gates **Qdrant payload trust** (the §2.1 pre-gate
  filter), a RESOURCE-axis signal. Bump it on:
  - Any `resource_grants` row whose `resource_id` is this document.
  - Any `resource_grants` row whose `resource_id` is an org_unit that
    intersects the document's `owner_org_unit_id` subtree (fan-out via
    closure).
  - Any `documents.owner_org_unit_id` change (reassignment).
  - An `org_unit` move's per-document fan-out (the move re-points which
    grants reach which documents). The move's company-epoch bump restores
    reader-scope correctness immediately; the per-document `acl_version`
    fan-out + deny-list + per-doc resync enqueue for the moved subtree is
    application-orchestrated in the move use-case (§2.5), not a trigger,
    because the affected-document set is a closure expansion and the
    deny-list is a Redis write. Correctness does not depend on it (G1 + the
    epoch hold the line); it is a recall optimization.

V032 enforces all of the above with `AFTER` triggers (same-tx, so G2 holds
for every code path, C++ or SQL): `resource_grants` and owner-change ->
`documents.acl_version` + resync enqueue (no epoch); `org_units`
insert/delete/move -> `companies.acl_epoch`; `memberships` / `group_members`
-> `users.scope_epoch`; plus `wikore_bump_company_acl_epoch()` for
break-glass.

**Payload (bump `kSchemaVersion` to 3).**

```cpp
std::int64_t acl_epoch     = 0;  // companies.acl_epoch at write time
std::int64_t acl_version   = 0;  // documents.acl_version at write time
```

**Pre-gate filtering (the actual use).** Before the gate query, the
candidate set is filtered with:

```
keep candidate iff:
    payload.acl_epoch   >= tenant_current_epoch
    AND payload.acl_version >= doc_current_acl_version(payload.document_id)
    AND payload.document_id NOT IN lr:acl:deny:{tenant}
```

Failures of any of these only *remove* the candidate from the
fast-path; they do not *add* anyone to the result set. Removed
candidates fall through to the gate query (which fetches the full
authoritative state for the affected document and re-evaluates). If
the gate says yes, the candidate is admitted; the overlay was simply
slower. **No leak**, by G1.

**Relationship to existing mechanisms (V008 / V015).**
`companies.acl_epoch` is the authoritative company-wide
scope-invalidation signal; V008's `lr:eff:keys:company:{C}`
reverse-index Sets are demoted to a best-effort eager-evict
optimization (crash-safe correctness lives in the monotonic epoch
compare, not in a DEL that can be lost). Per-document rewrite jobs
ride the existing V015 outbox - idempotent, coalesced by its UNIQUE
`(company_id, job_type, idempotency_key)`, ordered by `created_at` -
carrying `acl_version` in the JSONB payload. Do not stand up a
second queue alongside V008's `lr:resync:q`.

### 2.2 Revocation overlay (Redis deny-list, TTL-based)

**Why.** Even though G1 prevents leaks, recall correctness for revokes
still matters: a chunk whose ACL set shrunk should stop appearing in
Qdrant's prefiltered results promptly, so the gate is not asked the
same question repeatedly only to say no. Bumping `acl_version` plus
the version-bump filter handles this for new candidates, but Qdrant's
prefilter doesn't read `acl_version` (that is a payload field, not
filterable in the index). The deny-list is the bridge during the
window between the version bump and the payload rewrite.

**Mechanism.**

- Redis Set: `lr:acl:deny:{company_id}` containing `document_id`
  strings (per-document, not per-chunk). A 10k-document subtree move
  adds 10k members, not hundreds of thousands of chunk ids. The
  pre-gate filter knows each candidate's document via the payload, so
  per-document granularity is sufficient; V008's grant model never
  revokes a single chunk independently of its document.
- **Populated** synchronously inside the same outbox-and-PG-transaction
  flow used for the version bump, when a grant is revoked, an org-unit
  is moved, or a document is reassigned.
- **Drained** by the outbox worker (see §2.3) once a document's
  `qdrant_synced_version` reaches its `acl_version` for all chunks of
  that document. Drain = `SREM document_id` from the set.
- **TTL** on every deny entry, set at insert via `SADD` + `EXPIRE`,
  bounded by the maximum acceptable resync lag (initial value: 15
  minutes; configurable). Eliminates the need for a precise sweeper
  and bounds the leak-impossible-but-recall-degrading "stuck in the
  deny-list forever" failure mode. If outbox lag genuinely exceeds the
  TTL, the worst case is a chunk's stale payload reaches the gate and
  is correctly re-validated against PG - just one G1 query slower per
  affected candidate.

**Pre-gate filtering use.** One pipelined `SMISMEMBER` per query over
the candidates' distinct document ids (typically far fewer than the
~50 chunk candidates, since candidates cluster by document).

**Fail-open to the PG gate.** If the deny-list Redis is unreachable,
the pre-gate filter skips the deny check and lets every candidate fall
through to G1. That is slower (more candidates hit PG) but fully
available and still correct: the deny-list is a fast-reject
optimization, not the security boundary, so losing it degrades
recall latency, never isolation. Rejecting the whole tenant would
turn a Redis blip into a tenant-wide search outage for no correctness
gain.

**Fail-open is valid under v2.3's option (a).** Because the gate resolves
the resource axis live from `resource_grants` (§0 G1), the deny-list is a
pure recall optimization on top of an authoritative gate: if it is
unreachable, candidates fall through to G1, which still rejects a revoked
chunk because the revoke is committed in `resource_grants`. The deny-list
is never the only thing covering the revoke-to-resync window, so fail-open
degrades recall latency, never isolation.

### 2.3 Out-of-order outbox via Postgres-side CAS

**Why.** The outbox worker is multi-instance and may process events
out of order under retry, partition, or a slow worker. If an older
payload-rewrite job (carrying `acl_version = 3`) lands in Qdrant after
a newer one (`acl_version = 4`), the visible payload is stale (still a
recall problem under G1, not a leak, but degrades the §2.1 fast-path).

Qdrant and Postgres are separate stores with no shared transaction.
The version stamped into the Qdrant payload can therefore never be a
compare-and-set authority by itself: there is always a window between
"read current `acl_version` from PG" and "write payload to Qdrant" in
which another writer can interleave, and Qdrant has no
conditional-on-payload update to close it.

**Protocol.** The `qdrant_synced_version` column on `documents` is the
Postgres-side monotonic truth. The worker, while holding the V008
per-document advisory lock (or equivalent, e.g. `pg_advisory_xact_lock
(hashtext('resync:doc:' || document_id))`):

1. Reads `acl_version` from `documents` (call it `v`).
2. Recomputes the chunk payloads from PG as-of that read.
3. Upserts the chunks to Qdrant with `acl_version = v` in each
   payload.
4. In one PG statement:
   ```sql
   UPDATE documents
   SET    qdrant_synced_version = $1
   WHERE  id                    = $2
     AND  company_id            = $3
     AND  acl_version           = $1
   RETURNING id;
   ```
   If 0 rows updated, the version moved during the Qdrant write;
   another writer raced in. Re-enqueue the rewrite job and retry.
5. On success, attempt to `SREM document_id` from
   `lr:acl:deny:{company}` (best-effort; the TTL backstops loss).

**Out-of-order arrivals** are handled at step (1): if the event's
`acl_version` is `< current.acl_version`, drop the event as
superseded (log INFO, increment a counter). The worker never writes
a payload with an `acl_version` older than the one currently in
`documents`, regardless of the order events arrive.

**Framing.** `acl_version` in the Qdrant payload is a *hint* (and a
filter discriminator for the §2.1 fast-path);
`documents.qdrant_synced_version` in Postgres is the *monotonic truth*
about what Qdrant currently advertises. The gate trusts neither for
authorization (G1) and uses `qdrant_synced_version` only to decide
whether a backfill is owed.

### 2.4 Stale-candidate metric

**Why.** Cache rot is the failure mode the overlay is trying to
prevent. The system should make it observable in a single number, not
require log diving.

```cpp
// Counter incremented inside EvidenceGate's rejection branch.
wikore_retrieval_pg_revalidation_failed_total{
    reason = "acl"
           | "lifecycle"
           | "sensitivity"
           | "deny_overlay"
           | "tenant_mismatch"
           | "version_fallthrough"   // payload version < doc current; G1 ran
}
```

A non-zero baseline is expected (lifecycle transitions, deliberate
revocations); a *rising* baseline relative to total queries indicates
either the outbox is lagging, the deny-list is growing, or payload
rewrites are not draining. Alert on the rate, not the absolute count.
A `deny_overlay` rejection that persists long after the resync SLA
indicates a stuck `SREM` (mitigated but not eliminated by the §2.2
TTL).

### 2.5 Thundering-herd shape for subtree operations

**Why.** A `move_org_unit` operation on a subtree of 10k documents
synchronously rewriting 10k Qdrant payloads is the wrong correctness
ordering: it leaves the system inconsistent for the duration, and the
per-payload write serialises against the embed pipeline.

**Pattern.**

1. **In the same PG transaction as the move** (G2):
   - Bump `companies.acl_epoch`.
   - For each affected document: bump `documents.acl_version`.
   - Enqueue one V015 outbox event per affected document
     (`job_type = 'qdrant_resync_chunk_acl'`), idempotency key
     `resync:{document_id}:{acl_version}`, payload carrying
     `acl_version`.
   - `SADD lr:acl:deny:{tenant} document_id` for each affected
     document (via outbox so the Redis write is atomic with the PG
     change; per-document, so the set grows by the document count,
     not the chunk count).
2. **Synchronously after commit:** correctness is already restored.
   Every reader's G1 query reads the post-commit snapshot and sees
   the new ACLs. The §2 overlay either bounces the candidate (epoch
   mismatch, version mismatch, or document_id in deny-list) or lets
   it fall through to G1, which rejects it anyway.
3. **Asynchronously, in bounded batches:** outbox worker rewrites
   chunk payloads per §2.3. When a document's `qdrant_synced_version`
   reaches its `acl_version`, `SREM` the `document_id` from the
   deny-list. Search performance degrades smoothly (more candidates
   fall through to G1) until the rewrite drains.

The bounded-batches knob is the outbox worker's existing concurrency
limit; nothing new to design.

### 2.6 Reader-axis durability (closes scenario F)

**Why.** The resource axis (which org_units a chunk is visible to)
has company epoch + per-doc version + deny-list + G1. The reader axis
(which org_units a user belongs to) was, in the v1 design, protected
only by best-effort Redis `SMEMBERS + DEL` (V008's reverse-index
Sets). A lost or partial DEL after a `memberships` revoke leaves the
user's `lr:eff` cache stale, and G1 with a stale reader scope is a
leak: the gate resolves the chunk's document as visible to some OU in
`stale_reader_scope` (live resource grants intersected with a stale
reader set) and returns chunks the revoked user shouldn't see.

The v2.0 fix - bumping `companies.acl_epoch` on every membership
change - was too coarse. Membership changes are common (team moves,
project rotations); a tenant-wide cache invalidation on each event
turns the company epoch into a hot signal, defeats its role as a
break-glass lever, and stampedes the resolver for what is usually a
one-user event.

**Three concerns, three primitives.**

The reader-axis problem is actually three distinct events with three
distinct budgets, and conflating them is what made v2.0 ugly.

| Concern                                       | Latency budget   | Mechanism                                                       |
|-----------------------------------------------|------------------|-----------------------------------------------------------------|
| Routine membership churn (scope re-resolve)   | <= 5 minutes     | Atomic Lua invalidation + lr:eff TTL                            |
| Authoritative scope re-resolve mid-session    | <= 5 minutes     | `users.scope_epoch` piggybacked on the identity cache           |
| Hard termination / offboarding (stop querying)| immediate        | Session kill: DEL lr:api_key:{hash} + lr:jwt:{jti} revoke (V008)|

These compose; each is independently sized.

**Tier 1: atomic Lua invalidation + TTL ceiling.**

V008 already specifies a reverse-index Set
`lr:eff:keys:user:{C}:{u}` that records which `lr:eff` keys belong
to user `u`. On a membership change, the current path is
`SMEMBERS + DEL`, which has two known failure windows: partial DEL
under network blip, and a crash between `SADD` and the deferred
`EXPIRE` that races against the read.

- Replace the `SMEMBERS + DEL` path with a single Lua script (or
  `MULTI/EXEC` transaction) so the read of the index Set and the DEL
  of every referenced key are atomic. Closes the partial-DEL window.
- Re-affirm the `lr:eff` 5-minute TTL as the **guaranteed staleness
  ceiling** for the routine path: a lost invalidation cannot survive
  past 5 minutes of wall-clock time. State this explicitly in the
  V008 reference and document it as the SLA the routine-churn path
  is sized against.

For a team-move event (Alice moves from Marketing to Sales), the
expected behaviour is: invalidation succeeds immediately and the next
read recomputes (microsecond effect); on Redis hiccup the worst case
is up to 5 minutes of stale Marketing membership before the TTL fires
and forces re-resolve.

**Tier 2: per-user `scope_epoch`, piggybacked on the identity cache.**

When a membership change occurs while the user has an active session,
the user keeps querying with cached credentials. The Tier 1 path
invalidates `lr:eff`, but the gate also needs to know which
*resolved scope* to use; if the cache miss recomputes against fresh
PG state, that's fine, but if a stale `lr:eff` survived the (atomic
but still best-effort) invalidation, the gate must have a second
signal to compare against. `users.scope_epoch` is that second signal.

```sql
-- V0xx in iter-2's migration window.
ALTER TABLE users
    ADD COLUMN scope_epoch BIGINT NOT NULL DEFAULT 1;
```

Bump points (per G2 - same-tx as the authoritative change):

- `memberships` insert/update/delete for the affected user.
- `group_members` insert/update/delete for the affected user (via
  the user(s) implied by the group).
- Hard revocation paths (termination, suspension) - although those
  are *also* session-killed, see Tier 3, and `scope_epoch` is the
  secondary safety net for a request that races the session kill.

**How the gate reads it (the factually-correct path).**

`src/auth.cpp` is already the V008 fast path: on a cache hit the
X-API-Key handler returns the cached `Identity` from
`lr:api_key:{hash}` without touching Postgres; the
`SELECT ... FROM users ... WHERE ...` runs only on cache miss
(every 5 min, or on rotation, or on V008 invalidation). The JWT path
reads `lr:jwt:{jti}` (a Redis GET) and never reads the `users` row
on the hot path at all.

The piggyback is therefore:

- Add `scope_epoch` to the `users` SELECT that runs on the
  identity-cache miss path. The fetched `Identity` blob carries
  `scope_epoch` alongside `email`, `display_name`, `is_admin`.
- The gate compares
  `(cached_company_epoch, cached_scope_epoch)` from the `lr:eff`
  entry against
  `(current_company_epoch_from_redis, principal.scope_epoch_from_identity)`.
  Mismatch on either evicts `lr:eff` and re-resolves.
- `principal.scope_epoch` is therefore *fresh to within the
  identity-cache TTL* (the same 5-min ceiling as Tier 1, not
  per-request authoritative). This is by design: the same ceiling
  applies to every reader-axis signal so the SLAs are consistent.

The identity-cache invalidation V008 already specifies on
membership/revoke (`DEL lr:api_key:{hash}`, `DEL lr:jwt:{jti}`) is
the best-effort fast path that closes the 5-min window sooner. If
that DEL is lost, the TTL backstops at 5 minutes.

**No new authoritative read path.** Doing a per-request PG read of
`users.scope_epoch` would buy sub-5-min scope re-resolution that the
domain does not require, at the cost of a round-trip per request,
and would partly re-create the caching problem we are trying to
escape. The 5-min ceiling is the right tradeoff precisely because
the urgent case (Tier 3) is handled by session revocation, not by
`scope_epoch`.

**Tier 3: session kill (immediate, already V008).**

Hard termination is not a scope-re-resolution problem; it is a
"stop this principal from querying at all" problem. V008 already
specifies this and it is checked per-request:

- API-key path: `DEL lr:api_key:{hash}` removes the cached identity;
  the next request misses the cache, the PG SELECT returns no row
  (or a row with `disabled_at IS NOT NULL`), the request is
  rejected. Independent of `lr:eff`, independent of `scope_epoch`.
- JWT path: add the `jti` to `lr:jwt:{jti}` (or the
  equivalent denylist V008 defines); the per-request JWT check
  rejects the token immediately. Independent of `lr:eff`,
  independent of `scope_epoch`.

This is the immediate, durable cutoff. `scope_epoch` is the safety
net for the narrow window between "termination committed in PG" and
"every cached identity for the principal is evicted from Redis", a
window that is already best-effort sub-second and is bounded at the
5-min identity TTL.

**V008 reverse-index Sets remain as best-effort eager-evict.** Lower
steady-state cache miss rate; not the correctness mechanism. They
are sized for performance, not safety.

**Cost summary.**

| Surface                  | Per-tenant impact                                |
|--------------------------|--------------------------------------------------|
| Company `acl_epoch`      | Bumped on grants/moves/break-glass only          |
| Per-user `scope_epoch`   | Bumped on membership/group-member/revoke         |
| Atomic Lua invalidation  | One redis script call per membership event       |
| `lr:eff` 5-min TTL       | Worst-case routine staleness window              |
| Identity-cache TTL       | Same 5-min ceiling for `principal.scope_epoch`   |
| Auth-read piggyback      | Zero extra round-trips (column added to cache-   |
|                          | miss SELECT; rides the identity-cache TTL)       |
| Session-kill on revoke   | One Redis DEL per identity (V008, already there) |

Neither tier introduces tenant-wide stampedes; both tiers compose
under G2 (every epoch bump is in the same Postgres transaction as
its authoritative change, so reader causality holds for free at the
moment the identity cache misses or is invalidated).

### 2.7 kSchemaVersion 3 backfill prerequisite

**Why.** PR #24 moved `ChunkPayload::kSchemaVersion` from 1 to 2 with
no resync worker. This change is 2 to 3 (adds `acl_epoch` and
`acl_version`). Old Qdrant points written before this change have no
`acl_epoch` or `acl_version` keys; glaze deserializes missing fields
to default `int64` zero, so the §2.1 pre-gate filter will treat the
entire pre-existing corpus as stale (`0 < current_acl_version`) and
every old candidate will fall through to the G1 gate.

**That is safe** (G1 is the authority; the gate evaluates correctly).
**It is not free** (the whole corpus runs in G1-fallback mode until
the backfill completes). For a tenant with 100k chunks that means
~100k extra G1 queries spread over the catch-up window.

**Mitigation.**

- A one-shot backfill migration enqueues
  `qdrant_resync_chunk_acl` outbox events for every active
  `document_versions` row, with `acl_version` = current
  `documents.acl_version` (which the schema migration initializes to
  1 for all rows). The outbox worker drains them at its existing
  rate.
- The metric `version_fallthrough` in §2.4 lets ops see the
  backfill draining: it climbs immediately after the migration, then
  decays toward the steady-state baseline as the rewrite catches up.

**Document this as an explicit migration prerequisite**, not a thing
the test suite is allowed to mock.

---

## 3. Resolved scenarios (formerly open questions)

The five scenarios in §3 of v1 are reproduced here with their
now-known resolutions. Scenario F is added per Opus's review; it is
the most important one.

### A. Move-then-revoke race - not a leak (G1)

W2 in flight for subtree S. W1 begins while W2's outbox draining is
partway done. W1's epoch bump is a no-op (W2 already bumped it). W1's
deny-list write `SADD`s the revoked document. W2's worker, holding a
snapshot of the pre-W1-revoke ACL, rewrites a payload with the *new*
`acl_version` but the *old* (still-permitted-by-W2)
`access_scope_ids`, and `SREM`s the deny-list entry W1 just placed.

**At the overlay layer**, the chunk now looks "clean": fresh
`acl_version`, not denied, payload `access_scope_ids` is the pre-W1
set. **At the gate layer (G1)**, this is not a leak. Sequence:

```
W1 (revoke) commits in Postgres at t1: resource_grants row gone, doc acl_version bumped.
W2 worker at t2 (> t1, snapshot from before t1): upserts Qdrant payload
       acl_version = new, access_scope_ids = OLD (still has the revoked OU).
W2 worker at t3: SREM lr:acl:deny -> chunk now looks "clean" to the overlay.
R reads at t4 (> t1):
   §2 pre-gate filter: passes (stale-wide payload, not denied).
   G1 query: resolves the resource axis live from resource_grants +
       org_unit_closure AS OF the t4 snapshot (NOT from the denormalized
       prefilter column). W1's revoke (committed at t1) IS visible: the
       grant row is gone, so arm 2 / arm 3 find no surviving grant.
   -> chunk DROPPED.
```

`X = G1 + G2`. W1's revoke is committed before W1 returns; any read
whose snapshot starts after t1 sees the revoke. The stale payload and
the prematurely-removed deny entry only decide whether the candidate
*reaches* the gate, never whether it *passes*.

The optional fix to also clean up the recall regression (W2's stale
payload lingering until the next §2.3 rewrite): make the §2.3 worker
re-read `acl_version` under the per-document advisory lock as in
§2.3's CAS protocol. The PG-side CAS catches the version moved during
the Qdrant write and re-enqueues. **Already in §2.3**, no extra
mechanism needed.

**Why the gate sees the revoke (v2.3 / option (a)).** The "chunk DROPPED"
step holds because the G1 query resolves the resource axis live from
`resource_grants` + `org_unit_closure` (§0), not from the denormalized
`qdrant_prefilter_scope_ids`. W1's revoke is committed in `resource_grants`
at t1; R's gate query at t4 > t1 reads a snapshot in which the grant row
is gone, so arm 2 / arm 3 find no surviving grant and the chunk is dropped
regardless of the stale Qdrant payload or a prematurely-removed deny entry.
This is exactly the property the column filter could not provide.

### B. Out-of-order outbox - serialize, don't transact

The §2.3 guard cannot be inside the Qdrant upsert "transaction"
because Qdrant has no transaction PG can join. The fix is the
PG-side CAS on `documents.qdrant_synced_version` (§2.3), bracketed
by the per-document advisory lock. This is the smallest correctness
mechanism that makes the *advertised* synced version monotonic and
the stale-overwrite self-correcting, without pretending Qdrant is
transactional.

**Correctness:** does not depend on B at all (G1). Recall: handled.

### C. Deny-list drained too early - acceptable false negative

Correct as v1 suspected. Rejecting a correctly-granted chunk because
the deny entry outlived the payload rewrite is a recall regression,
not a security violation. Bounded by: the `SREM` runs in the same
worker step immediately after the §2.3 CAS succeeds, and the §2.4
`deny_overlay` counter makes a stuck `SREM` visible.

### D. Deny-list drained too late - TTL, no sweeper

The proposed sweep predicate ("chunk in deny-list where
payload.acl_version >= current for all access_scope_ids") is
expensive and the wrong shape. Because a lingering deny entry is only
a recall loss (G1 still holds the security line), no precise sweeper
is needed.

Give every deny entry a TTL equal to the maximum acceptable resync
lag (15 min, configurable). On expiry it vanishes. If resync genuinely
has not completed by then, the chunk's stale payload reaches the gate
and is re-validated - correct, just one query slower per affected
candidate. A TTL removes the crash-recovery sweeper, the orphan-entry
class of bug, and the unbounded memory growth in one move. Already
in §2.2.

### E. Stale reader scope - single-statement resolver

Two parts.

1. **Does observing epoch N+1 imply the revoke is visible?** Yes,
   under G2: the epoch bump is in the same transaction as the
   authoritative change, so under Read Committed any reader that sees
   the new epoch is reading a snapshot in which the change is
   committed. Causality holds for free; no extra locking needed.

2. **Phantom / torn read during a concurrent `move_org_unit` (V012).**
   `move_org_unit` is a single atomic transaction, so it is invisible
   or fully visible to any *single* statement. The danger is only if
   the resolver reads its inputs across *multiple* statements (read
   memberships, then read grants, then read closure in separate
   round-trips): the move can commit between them and the resolver
   stitches a pre-move membership set onto a post-move closure,
   producing a scope that never existed. **Fix:** resolve the
   effective scope in a single SQL statement (one query joining
   `memberships`, `resource_grants`, and `org_unit_closure`). One
   statement = one Read Committed snapshot = cannot tear. If a single
   statement is impractical, wrap the re-resolution in `BEGIN ISOLATION
   LEVEL REPEATABLE READ ... COMMIT` so all reads share one snapshot.
   REPEATABLE READ may yield the pre-move scope, which is fine (a
   consistent point in time); what is forbidden is the torn mix that
   multi-statement Read Committed allows. **Already required by §1
   item 1.**

### F. Reader-axis lost invalidation - the deepest issue (closed by §2.6)

Without §2.6, this is a leak:

```
Alice is in HR. lr:eff:{C}:{alice}:{...} = [HR, ...] cached.
Admin terminates Alice (hard offboarding: membership revoke +
   account suspension).
   v1 / v2.0 paths: best-effort SMEMBERS + DEL of lr:eff keys.
   Neither bumped a per-user signal Alice's cache had to compare against.
The DEL fails or partially fails (Redis hiccup, reverse-index Set already
   expired, crash between SADD and the deferred EXPIRE).
Alice's next request:
   epoch check (v1, v2.0 with company-epoch-on-membership): epoch unchanged
       or not granular enough to matter -> lr:eff NOT discarded
       -> stale [HR, ...] used.
   G1 query: chunk granted to HR; reader_scope (stale) contains HR.
   -> chunk RETURNED. LEAK.
```

The overlay does not catch this because the chunk's resource-axis
state is entirely correct; Alice's cached scope wrongly says she is in
HR. None of `acl_epoch` (resource axis), `acl_version`, or the
deny-list reference the *reader* axis.

**Fix (§2.6).** Three composing primitives, each handling a distinct
concern:

1. **Routine scope re-resolution (≤5 min ceiling, common case):**
   replace V008's `SMEMBERS + DEL` path with an atomic Lua script so
   the index-read and the DEL of every referenced `lr:eff` key
   cannot tear. State the `lr:eff` 5-minute TTL as the *guaranteed
   staleness ceiling*: a lost invalidation cannot survive past 5
   minutes regardless of any failure mode. Acceptable for a
   team-move event.

2. **Authoritative scope re-resolve mid-session (≤5 min ceiling,
   second signal):** add `users.scope_epoch BIGINT`, bumped in the
   same transaction as the membership/group-member/revoke change
   (G2). Every `lr:eff` value carries the `(company_acl_epoch,
   user_scope_epoch)` tuple it was computed under; the resolver
   compares both before serving. Mismatch on either = discard +
   re-resolve. `users.scope_epoch` is read by piggybacking on the
   existing identity-cache miss path in `auth.cpp` (the X-API-Key
   SELECT and the JWT-issue path); the fetched `Identity` blob
   carries `scope_epoch`. **The freshness ceiling is therefore the
   identity-cache TTL (5 min), not request-time authoritative.**
   This is by design: it matches the Tier-1 ceiling, costs zero
   extra round-trips per request, and does not re-create the
   per-request caching problem we are trying to escape.

3. **Termination (immediate, V008 session kill):** the "Alice must
   stop querying *now*" event is not a scope-re-resolution problem,
   it is a session-revocation problem. V008 already handles it on
   the per-request path: `DEL lr:api_key:{hash}` removes the cached
   identity, so the next request misses the cache, the PG SELECT
   returns no row (or a row with `disabled_at IS NOT NULL`), and
   the request is rejected. `lr:jwt:{jti}` is the equivalent
   for the JWT path. Both are checked per-request, both are
   independent of `lr:eff` and `scope_epoch`. This is the immediate,
   durable cutoff; `scope_epoch` is the safety net for the narrow
   window between "termination committed in PG" and "every cached
   identity is evicted from Redis".

Read this composition as: the boundary against a *leak* is G1
(authoritative PG re-check). The boundary against a *terminated
user continuing to query* is V008 session-kill (per-request identity
check). `scope_epoch` is the reader-axis equivalent of the
resource-axis `acl_version` - it keeps the cached scope in sync with
authoritative state within the same 5-min ceiling the rest of the
reader-axis already sits on.

The company epoch stays the break-glass lever it was meant to be
(resource axis only); per-user `scope_epoch` handles per-user
membership change surgically within the identity-cache ceiling;
atomic Lua + TTL covers the common membership-change path without
stampeding the tenant; session-kill handles termination immediately
on its own per-request path.

---

## 4. Things deliberately deferred to Iter 3

- The actual reranker (`authority_level`-weighted; `AllowedCandidate`
  in, ranked `AllowedCandidate` out). Iter 2 produces the candidate
  set; ranking inside it is iter 3.
- The chat endpoint and LLM streaming. Iter 2 is read-path primitives
  only.
- The resync worker for org-tree moves at very large scale (>100k
  documents per move). §2.5's pattern works at the 10k scale; >100k
  may need a different shape (e.g. partition the deny-list across
  multiple Redis keys, batch outbox enqueue beyond V015's normal
  rate).
- Cross-region replication. Out of scope for self-hosted single-region
  deployments; revisit if multi-region becomes a requirement.
- User/group-specific grants (the `access_tokens` model; V002 defers
  this - grants are `principal_type='org_unit'` only in the MVP). When it
  lands, G1's reader-side-inversion cost model (§0 item 1, §5 item 5) must
  be re-derived: user/group principals add a fan-out the current
  closure-only bound does not cover.

---

## 5. Acceptance for this design before Iter 2 coding starts

The acceptance criteria are expressed against G1, not against the
overlay's internal invariant. This is the test a security reviewer
will ask for, and it keeps passing even if the overlay is later
simplified or removed.

1. **G1 subset property test. DONE.** `tests/test_retrieval_invariants.cpp`
   fuzzes randomized tenant configurations (random tree, grants,
   memberships, documents) and for every reader asserts the gate equals
   an INDEPENDENT oracle - `fetch_access_scopes` (resource-side expansion)
   intersected with `effective_read_orgs` (reader-side scope). Because the
   gate resolves live PG, safety (`gate ⊆ pg_truth`, no leak) and liveness
   (`gate = pg_truth`) collapse to one equality check per config. 40 trials
   / 945 assertions pass against Postgres 17; the test was confirmed to
   fail with an OVER-GRANT on the pre-fix query (see the arm-2/arm-3
   `principal_applies_to` note in §0), so it has teeth. The W1/W2/W3
   prefilter-staleness interleaving layer is added on top once EvidenceGate
   and the §2 overlay exist.

2. **Scenario F regression test.** Forced membership-revoke with a
   mocked Redis drop on the `lr:eff:keys:user` DEL must not return
   HR chunks to the removed user. Under §2.6 this becomes a test
   that an unbumped or lost reverse-index DEL is backstopped by the
   epoch check; the test should fail if §2.6's epoch coverage of
   membership changes is ever removed.

3. **Scenario E torn-read test.** Concurrent `move_org_unit` during
   a scope re-resolution must yield either the pre-move or post-move
   scope, never a mix. Easiest assertion shape: instrument the
   resolver to return the scope set; run 1000 trials of "start
   resolver in thread A, start move in thread B" with random delays;
   assert the resolver output is in the set
   `{pre_move_scope, post_move_scope}` for every trial.

4. **Schema migration draft.** §2.1's `ALTER TABLE` statements (the
   `acl_epoch` / `acl_version` / `qdrant_synced_version` columns, the
   `users.scope_epoch` column, and the `access_scope_ids ->
   qdrant_prefilter_scope_ids` rename with its GIN index) plus the §2.7
   backfill outbox-enqueue procedure are written as V032+ (V031 is already
   taken by partition_ensure_fn) alongside this document. The migration is
   not part of this document; this document stays a design note.

5. **Gate-query cost benchmark (gates adoption of option (a)). DONE - see
   result below.** `EXPLAIN ANALYZE` the gate query on a synthetic
   worst-case tenant: ~60 candidates over ~30 documents, `reader_scope`
   ~500 OUs whose owners are not in scope (so arm 1 misses), tree depth 10,
   ~10k `resource_grants` including one org_unit resource with 1k principal
   grants. The two stress cases are all-visible (every candidate passes,
   EXISTS short-circuits) and **all-reject** (every candidate must be
   rejected, so the resolver cannot short-circuit and scans the grant set -
   this is the true upper bound; in production it is the stale-overlay
   candidates that reached the gate). Pass condition: p99 < 10ms.

   **Result (Postgres 17, warm cache):**

   | Gate query form                          | all-visible | all-reject |
   |------------------------------------------|-------------|------------|
   | Correlated per-candidate EXISTS (naive)  | ~2.7 ms     | ~31-48 ms  |
   | Set-based, distinct docs resolved once   | ~6.7-7.9 ms | ~5.3-6.8 ms|

   **Finding: option (a) is viable, but only with the SET-BASED gate query.**
   The correlated-EXISTS form (one EXISTS per candidate row, as the §0 Step B
   sketch reads) makes Postgres re-scan `resource_grants` once per candidate
   (`Seq Scan ... loops=30`, ~300k rows) on the all-reject path, landing at
   3-5x over budget. Resolving the **distinct candidate documents (~30) in a
   single set-based pass** - `cand_docs` CTE, then `visible` = arm1 UNION
   arm2-join UNION arm3-join against `reader_grant_keys`, scanning the grant
   table once via hash joins - holds both the common and worst case under
   10ms. The canonical query is recorded in §0 (the Step B sketch is marked
   illustrative-only).

   **Regression threshold to watch:** cost scales with (distinct candidate
   docs) x (grants on the candidates' owner-ancestor resources). At ~30 docs
   / 10k grants / 1k-fan-out resource the set-based form is ~7ms; budget is
   reached around a 3-4x increase in either factor. Re-benchmark when the
   `access_tokens` user/group grant model lands (§4), which adds a principal
   fan-out the closure-only bound does not cover.

Once those five land, Iter 2 implementation can begin. Anything that
contradicts G1 or G2 during implementation is a bug and reverts to
this document for adjudication.

---

## Appendix: changelog

### v2.0 - integrated Opus's first review pass

- §0 added: G1 (gate authority) and G2 (same-tx epoch bump) named as
  the two load-bearing invariants. Everything in §2 reframed as
  optimization under G1.
- §1 item 7: as-of reads bypass the live overlay; resolve from V024
  history.
- §2.1: schema unchanged from Opus's v1 edit (column on `documents`,
  not a side table; `qdrant_synced_version` column added).
- §2.2: deny-list now per-*document*; fail-*open*; TTL on every entry
  (D resolution).
- §2.3: rewritten as Postgres-side CAS on `qdrant_synced_version`
  bracketed by per-document advisory lock; out-of-order events drop
  at version check.
- §2.4: `version_fallthrough` reason added to the metric label set.
- §2.5: enqueue is per-*document* outbox event with idempotency key
  `resync:{document_id}:{acl_version}`.
- §2.6 added: reader-axis epoch coverage (initial v2.0 design bumped
  `companies.acl_epoch` on membership/group-member changes; superseded
  in v2.1).
- §2.7 added: kSchemaVersion 3 backfill prerequisite.
- §3: rewritten from open questions to resolved answers, each citing
  G1/G2 or the relevant §2 subsection.
- §5: rewritten around the G1 subset property + F + E tests, not the
  overlay's internal invariant.

### v2.1 - integrated Opus's second review pass (per-user scope_epoch)

- §2.1 bump points: `companies.acl_epoch` is RESOURCE-AXIS ONLY now.
  Removed `memberships` and `group_members` bump entries. The company
  epoch goes back to being a rarely bumped break-glass lever, not a
  routine-churn signal. Avoids tenant-wide cache stampedes for
  one-user events.
- §2.6 rewritten as two-tier (atomic Lua + TTL for routine churn;
  per-user `scope_epoch` for hard revoke). Asserted (incorrectly)
  that the auth path does a per-request PG read of `users`, so
  `principal.scope_epoch` could be treated as request-time
  authoritative.
- §3.F resolution rewritten to point at the two-tier mechanism.

### v2.2 - integrated Opus's third review pass (auth-path correction)

The factual claim in v2.1 §2.6 about "zero extra round-trips and
visibility on the next request after commit" was wrong: `auth.cpp`
caches the identity in `lr:api_key:{hash}` (5-min TTL, V008
invalidation on revoke) and the JWT path never reads the `users` row
on the hot path at all. So `principal.scope_epoch` rides the
identity-cache TTL, which is also 5 minutes - the same ceiling as
`lr:eff`. This actually clarifies the design rather than complicates
it: every reader-axis signal sits on the same 5-min ceiling, and
"immediate revocation" was always going to need a different
mechanism than a cached value.

- §2.6 reorganised from two tiers to **three concerns / three
  primitives**:
  - Routine membership churn (atomic Lua + lr:eff TTL).
  - Authoritative scope re-resolve mid-session (`users.scope_epoch`
    piggybacked on the identity cache; freshness ceiling = identity
    TTL = 5 min).
  - Hard termination / offboarding (session kill via
    `DEL lr:api_key:{hash}` + `lr:jwt:{jti}`, already V008
    per-request; independent of `lr:eff` and `scope_epoch`).
- §2.6 corrected: `principal.scope_epoch` is fresh to within the
  identity-cache TTL, not request-time authoritative. The auth-read
  piggyback works on the cache-miss path; cached identities reuse
  the cached `scope_epoch` until the identity cache misses or is
  invalidated.
- §3.F resolution rewritten to reflect the three-primitive split:
  G1 is the leak boundary, V008 session-kill is the
  terminated-user-cannot-query boundary, `scope_epoch` is the
  in-session scope re-resolve.
- No schema change from v2.1: `users.scope_epoch` is still the only
  new column. No new read path; the migration surface stays narrow.

### v2.3 - integrated Opus's fourth review pass (resource-axis resolution)

Closes the §0 OPEN item raised in the v2.2 review: the G1 query filtered
the resource axis on the denormalized `dc.access_scope_ids`, which is
recomputed only by the async resync worker (V002/V003/V008, no synchronous
trigger), so it was exactly as stale as the Qdrant payload and scenario A
leaked. Resolution: **option (a)**, resolve the resource axis live.

- §0 G1: the resource-axis check is now a live resolve from
  `resource_grants` + `org_unit_closure`, written as a **reader-side
  inversion** (compute `reader_grant_keys` once per request, then three
  EXISTS arms per candidate document) rather than the ingest-style
  per-document subtree expansion. The one-snapshot property now explicitly
  covers `resource_grants`, which is what makes scenario A safe.
- §0 / §2.1: `document_chunks.access_scope_ids` is demoted to a Qdrant
  prefilter hint and **renamed** `qdrant_prefilter_scope_ids` (V032), GIN
  index renamed with it. It stays load-bearing for the write path (Qdrant
  payload source + resync targeting) only; the gate never reads it.
  Confirmed the only consumers of the PG column are `outbox_worker.cpp`
  (payload source) and the GIN index - no analytics, audit, or admin
  reader - so the demotion is clean.
- §1 item 3: the Qdrant prefilter still matches the resolved scope set,
  not `owner_org_unit_id` alone (owner-only would miss grant-only
  visibility = index-level false negatives). Prefilter staleness is now a
  pure recall concern under (a).
- §2.2 / §3.A: the v2.2 "depends on §0 resolution" caveats are discharged;
  fail-open is valid and scenario A's "chunk DROPPED" is justified because
  the gate reads live grants.
- §5 item 5 added: cost-threshold benchmark (`EXPLAIN ANALYZE` on a
  worst-case synthetic tenant) with a named regression threshold, gating
  adoption of (a).
- §4: user/group (`access_tokens`) grants noted as the point at which
  (a)'s cost model must be re-derived.

**Option (c) - write-trigger-with-tombstone - considered and dismissed.**
(c) keeps the denormalized column as the gate's source but has
grant-change functions write a per-chunk `dirty` boolean in the same
transaction; the gate trusts the column when `dirty=false` and
live-resolves when `dirty=true`; the resync clears `dirty`. It is
dominated:
- **Same row fan-out as (b).** An org-unit grant affecting 10k chunks
  writes 10k rows either way; (c) writes a narrower column but touches the
  identical row count in the grant transaction. The write storm it claims
  to avoid is the row count, which it keeps.
- **Still needs (a)'s query.** For `dirty=true` chunks the gate must
  live-resolve - the exact arm-1/2/3 query - so (c) = (a)'s read path +
  (b)-style write fan-out + a new persistent state + a new failure mode
  (stuck `dirty=true`).
- **Its only unique property is harmful:** "trust the column when
  `dirty=false`" re-introduces "the gate trusts a denormalized column,"
  the precise authority-confusion (a) removes.
- **Its legitimate kernel is already provided.** "Only live-resolve when
  the doc might be stale" is exactly (a) + the §2.1 `acl_version` fast-path
  + the deny-list: under (a) the gate's live-resolve runs only for
  candidates that fail the version fast-path or sit in the deny-list. The
  overlay is the clean tombstone; a per-chunk `dirty` column is a redundant
  second one with worse write and failure properties.

No schema beyond v2.1's columns plus the V032 rename: `acl_epoch`,
`acl_version`, `qdrant_synced_version`, `users.scope_epoch`, and
`access_scope_ids -> qdrant_prefilter_scope_ids`.

**Bump-point correction (during V032 drafting).** §2.1's bump list
previously bumped `companies.acl_epoch` on "any resource_grants
insert/update/delete" and on owner-change. That was wrong: `acl_epoch`
gates the `lr:eff` reader-scope cache, and grant/owner changes are
resource-axis events that never alter a user's resolved scope, so bumping
the company epoch there would stampede every tenant `lr:eff` for nothing
(the membership-side pathology §2.6 had already removed). §2.1 now bumps
`acl_epoch` only on org structural changes (create/delete/move) +
break-glass, and routes grants/owner-changes to `documents.acl_version`.
V032 implements this split with AFTER triggers and it is verified against
Postgres 17: a grant insert bumps `acl_version` + enqueues a resync and
leaves `acl_epoch` unchanged; org-unit insert/move bumps `acl_epoch`;
membership/group-member bumps `users.scope_epoch`; the backfill enqueue is
idempotent. The `document_repo.cpp` / `outbox_worker.cpp` write path was
renamed to `qdrant_prefilter_scope_ids` in the same change.

**Gate-query `principal_applies_to` fix (found by the §5-item-1 property
test).** The set-based §0 query matched `principal_id` against
`reader_grant_keys` (reader_scope + ancestors) for every grant, silently
treating all grants as `self_and_descendants`-principal. That over-grants a
`self_only`-principal grant to a reader sitting in a descendant of the
principal - an over-grant, i.e. a leak. Arms 2 and 3 now split on
`principal_applies_to`: `self_only` matches against `reader_scope`,
`self_and_descendants` against `reader_grant_keys`. The property test
(`tests/test_retrieval_invariants.cpp`) compares the gate to an independent
oracle and was confirmed to fail on the pre-fix query, so the boundary is
now regression-guarded. The §5 item-5 benchmark numbers are unaffected (the
fix is the same query shape).
