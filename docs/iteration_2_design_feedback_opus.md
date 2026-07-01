# Iteration 2 Design - Review (Opus 4.8)

**Reviewing:** `docs/iteration_2_design.md`
**Verdict:** The baseline (Section 1) is sound and the refinements (Section 2)
are well-motivated, but the document has one structural problem that, once
fixed, dissolves most of Section 3's hard questions. Scenarios A, B, C, D are
recall/observability problems, not leak channels, **provided** one invariant is
held as load-bearing. Scenario E is a real correctness question with a small
fix. There is a sixth scenario (the reader axis) that is more important than
A-E and is not covered by any Section 2 machinery.

This review is ordered: (0) the headline, (1) the load-bearing argument,
(2) scenarios A-E with smallest fixes, (3) the sixth scenario, (4) schema and
design-shape notes, (5) changes to the acceptance criteria.

---

## 0. Headline: do not let the overlay become the security boundary

Section 1 item 6 states the real invariant of this iteration:

> **Qdrant is the index, Postgres is the evidence.**

and item 5 says the EvidenceGate conversion `ChunkCandidate -> AllowedCandidate`
"runs the Postgres re-validation (lifecycle, sensitivity, scope membership,
tenant); anything that fails to convert is dropped."

If that re-validation is authoritative and unconditional, then **the Qdrant
payload, the `acl_epoch`, the `acl_version`, and the deny-list can all be
arbitrarily stale and still never produce a leak.** A stale prefilter can only
ever drop a candidate that should have been returned (a false negative on
recall). It cannot manufacture a false positive, because the gate re-checks
every survivor against authoritative Postgres before it becomes an
`AllowedCandidate`.

Section 2 then builds an elaborate resource-axis staleness system on top of
this and, in 2.2, makes it **fail-closed as if it were the boundary**
("If the deny-list Redis is unreachable, the EvidenceGate must reject every
candidate from the affected tenant"). That inverts the layering. A Redis
outage should degrade to "every candidate falls through to the PG gate" -
slower, fully available, still correct - not "the tenant cannot search."

**The single most valuable change to this document:** reframe all of Section 2
as a *recall and performance optimization that reduces how many candidates
reach the PG gate*, and state explicitly that none of it is on the correctness
path. Once you do that, A/B/C/D stop being leak questions and become "how fast
does recall recover" questions, which have cheap answers.

The rest of this review assumes that reframing and shows what each scenario
looks like under it.

---

## 1. The load-bearing argument, stated precisely

Make this the wall everything else leans on, and write it into the doc as an
invariant with a name (call it **G1**):

> **G1 (gate authority).** No `ChunkCandidate` becomes an `AllowedCandidate`
> except by passing a single authoritative Postgres query, evaluated under one
> snapshot, that re-checks: tenant match, lifecycle = active (or the as-of
> clamp), sensitivity policy, and `chunk.access_scope_ids INTERSECT
> reader_scope`. The Qdrant payload is an input to *candidate selection only*,
> never to *authorization*.

Two properties make G1 cheap enough to actually be the boundary:

1. **It is one query, not N.** Given the reader's resolved scope set and the
   ~50 candidate chunk_ids, the gate is a single set-returning statement
   (`WHERE company_id = $1 AND lifecycle = 'active' AND sensitivity ... AND
   access_scope_ids && $reader_scope AND chunk_id = ANY($candidates)`). The
   per-candidate PG round-trip that the deny-list is trying to avoid does not
   exist if the gate is written as one batched query. This substantially
   weakens the performance justification for the deny-list.

2. **It uses one snapshot.** The resource-axis re-check must read grants,
   memberships, and closure in a single statement (or a single REPEATABLE READ
   transaction), so it cannot observe a torn state during a concurrent move.
   See scenario E.

With G1 in place, here is each Section 3 scenario.

---

## 2. Scenarios A-E

### A. Move-then-revoke race - NOT a leak. "Cannot happen because G1."

The author's worry: W2's worker rewrites a payload with the *new* `acl_version`
but the *old* `access_scope_ids`, and `SREM`s the deny-list entry W1 just
placed, leaving a chunk that is fresh-versioned, not-denied, and carrying a
stale-wide ACL.

That is all true at the *overlay* layer. It is not a leak, because the reader
never receives the chunk on the strength of the overlay. Sequence:

```
W1 (revoke) commits in Postgres at t1: resource_grants row gone, doc acl_version bumped.
W2 worker at t2 (> t1, snapshot from before t1): upserts Qdrant payload
       acl_version = new, access_scope_ids = OLD (still has the revoked OU).
W2 worker at t3: SREM lr:acl:deny -> chunk now looks "clean" to the overlay.
R reads at t4 (> t1):
   prefilter: passes (stale-wide payload, not denied)         <- overlay says "maybe"
   EvidenceGate G1 query: reads resource_grants AS OF t4 committed snapshot.
       W1's revoke (committed at t1) IS visible.
       chunk.authoritative access_scope_ids does NOT intersect R's scope.
   -> chunk DROPPED.                                            <- boundary says "no"
```

X = **G1, plus the fact that W1's revoke is committed before W1 returns.** Any
read whose snapshot starts after t1 sees the revoke. The stale payload and the
prematurely-removed deny entry only decide whether the candidate *reaches* the
gate, never whether it *passes*.

If you reject G1 and insist the deny-list/acl_version be authoritative, then A
*is* a leak and the smallest fix is ugly (W2's worker must re-read
`acl_version` under a per-document row lock and abort if it changed since its
snapshot, and W1 must take the same lock). Do not go there. Keep G1.

### B. Out-of-order outbox - the guard cannot be transactional; serialize instead.

The question asks whether the 2.3 guard (`event.acl_version < current => drop`)
can sit inside the Qdrant upsert transaction. **It cannot: Qdrant and Postgres
are separate stores with no shared transaction.** So the version stamped into
the Qdrant payload can never be a compare-and-set authority; there is always a
window between "read current acl_version from PG" and "write payload to Qdrant"
in which another writer can interleave, and Qdrant has no conditional-on-payload
update to close it.

Two consequences:

1. **Correctness:** does not depend on B at all, by G1. A stale Qdrant payload
   (wrong version, wrong ACL) is caught by the gate. So B is a recall problem.

2. **Recall:** to stop a stale rewrite from durably overwriting a fresh one,
   serialize rewrites per document using the lock V008 already defines
   (`lr:resync:lock:{company_id}:{org_unit_id}` - tighten it to per-document or
   add `lr:resync:lock:doc:{doc_id}`). The worker, while holding the lock:
   re-reads `acl_version = v`, recomputes the payload from PG as of that read,
   upserts to Qdrant, then in one PG statement does
   `UPDATE documents SET qdrant_synced_version = v WHERE id = $doc AND
   acl_version = v`. If 0 rows updated, the version moved during the Qdrant
   write; re-enqueue. This is a Postgres-side compare-and-set bracketing the
   non-transactional Qdrant write: it makes the *advertised* synced version
   monotonic and the stale-overwrite self-correcting, without pretending Qdrant
   is transactional.

The clean framing for the doc: **the acl_version in the payload is a hint;
`documents.qdrant_synced_version` in Postgres is the monotonic truth; the gate
trusts neither for authorization (G1) and uses `qdrant_synced_version` only to
decide whether a backfill is owed.**

### C. Deny-list drained too early - acceptable false negative.

Correct as the author suspects: rejecting a correctly-granted chunk because the
deny entry outlived the payload rewrite is a recall regression, not a security
violation. The Section 3 invariant is a no-leak (no false-positive) property;
it says nothing about recall. C is fine **if bounded**: the `SREM` must run in
the same worker step immediately after the Qdrant upsert ack, and the 2.4
metric must count `deny_overlay` rejections so a stuck `SREM` is visible. Add
one line to the doc: the deny-list is allowed to cause false negatives but must
never cause them for longer than the resync SLA.

### D. Deny-list drained too late - delete the sweeper, use a TTL.

The proposed sweep predicate ("chunk in deny-list where payload.acl_version >=
current for all access_scope_ids") is expensive and the wrong shape. Because a
lingering deny entry is only a recall loss (G1 still holds the security line),
you do not need a precise sweeper at all:

**Give every deny entry a TTL equal to the maximum acceptable resync lag (say
15 min).** On expiry it vanishes. If resync genuinely has not completed by
then, the worst case is that the chunk's stale payload reaches the gate and is
re-validated against PG - correct, just one query slower. A TTL removes the
crash-recovery sweeper, the orphan-entry class of bug, and the unbounded memory
growth in one move. If you want belt-and-suspenders, the per-document
reconciliation (`SREM` when `qdrant_synced_version >= deny_entry_version`) can
run as a cheap side effect of the rewrite worker, keyed by document, not as a
periodic full scan.

### E. Stale reader scope - real correctness question, small fix.

Two parts.

1. **Does observing epoch N+1 imply the revoke is visible?** Yes, *if and only
   if* the epoch bump is in the same transaction as the authoritative grant or
   membership change. The doc says it is (2.1, "in the same transaction as any
   grant ... mutation"). Pin this as an invariant **G2**: *the epoch and
   per-document version are bumped in the same Postgres transaction as the
   authoritative change they describe.* Under Read Committed, a reader that
   observes epoch N+1 is reading a snapshot in which W1's whole transaction
   (epoch bump + revoke) is committed, so the re-resolve sees the revoke.
   Causality holds for free; no extra locking needed.

2. **Phantom/torn read during a concurrent move (W2).** This is the real risk.
   `move_org_unit` (V012) is a single atomic transaction, so it is invisible or
   fully visible to any *single* statement. The danger is only if the resolver
   reads its inputs across *multiple* statements (read memberships, then read
   grants, then read closure in separate round-trips): W2 can commit between
   them and the resolver stitches a pre-move membership set onto a post-move
   closure, producing a scope that never existed. **Smallest fix: resolve the
   effective scope in a single SQL statement** (one query joining memberships,
   resource_grants, and org_unit_closure). One statement is one Read Committed
   snapshot and cannot tear. If a single statement is impractical, wrap the
   re-resolution in a REPEATABLE READ transaction so all reads share one
   snapshot. Note that REPEATABLE READ may yield the *pre-move* scope - that is
   fine (a consistent point in time); what you must forbid is the torn mix that
   multi-statement Read Committed allows.

---

## 3. The sixth scenario (F) - the reader axis is unprotected

The author asked for a scenario more important than A-E. Here it is.

**Every mechanism in Section 2 hardens the resource axis** (which org_units a
chunk is visible to: epoch, doc acl_version, deny-list, payload rewrites). The
**reader axis** (which org_units a user belongs to: `lr:eff`) is protected only
by best-effort Redis `SMEMBERS+DEL`, per the V008 invalidation table, which
explicitly classifies membership changes as "No Qdrant resync, just DEL the
user's eff keys."

Consider:

```
Alice is in HR. lr:eff:{C}:{alice}:{oid} = [HR, ...] cached.
Admin removes Alice from HR (membership revoke).
   V008 path: SMEMBERS lr:eff:keys:user:{C}:{alice} -> DEL those keys.
   This does NOT bump companies.acl_epoch (2.1 lists "grant or org-tree
   mutation"; a pure membership revoke is neither, and V008 does not bump it).
The DEL fails or partially fails (Redis hiccup, reverse-index Set already
   expired at the 30-min mark, crash between SADD and EXPIRE).
Alice's next request:
   epoch check: epoch unchanged -> lr:eff NOT discarded -> stale [HR, ...] used.
   EvidenceGate G1 query: chunk granted to HR; reader_scope (stale) contains HR.
   -> chunk RETURNED. LEAK.
```

The overlay does not catch this because the chunk's resource-axis state is
entirely correct - the chunk really is granted to HR, and Alice's *cached*
scope wrongly says she is in HR. None of acl_epoch, acl_version, or the
deny-list references membership.

**This is the deepest issue in the design.** Two ways to fix it, in order of
preference:

1. **Make G1 re-resolve both axes, or epoch-guard the reader axis too.**
   Either the gate re-derives Alice's scope from authoritative `memberships`
   (not from `lr:eff`) on the read path - expensive but truly authoritative -
   or, cheaper, **bump `companies.acl_epoch` on membership and group-member
   changes as well**, and make `lr:eff` carry and check the epoch exactly like
   the resource-axis cache. Then a membership revoke invalidates the reader
   cache crash-safely (monotonic int compare) instead of relying on a DEL that
   can be lost. This is the same crash-safety argument the doc already makes
   for the resource axis in 2.1; apply it symmetrically.

2. At minimum, **state explicitly that `lr:eff` correctness depends on the
   reverse-index DEL never being lost**, and treat that as a security
   dependency with the same fail-closed scrutiny 2.2 applies to the deny-list.
   Right now the asymmetry is silent: the resource axis gets epoch + version +
   deny-list + fail-closed, the reader axis gets a best-effort DEL.

I recommend option 1 with the epoch covering membership changes. It unifies the
two axes under one monotonic-epoch discipline and removes the lost-DEL leak
class entirely. Note this raises epoch churn (memberships change more often than
grants), so size the epoch as the cheap fast-path it is and keep the actual
scope recompute lazy (on next read after epoch advance).

---

## 4. Schema and design-shape notes

1. **Reconcile with V008 and V015 instead of paralleling them.** The doc
   introduces `lr:acl:deny:{company_id}` and a `qdrant_resync_chunk_acl` event,
   but V008 already specifies `lr:resync:q` and a precise invalidation table,
   and V015 already gives an idempotent, coalescible (UNIQUE on
   `idempotency_key`), `created_at`-ordered outbox keyed by `aggregate_id`.
   Carry `acl_version` as a field on the existing outbox payload; do not stand
   up a second queue. Fold the new deny-list and version columns into the V008
   reference and the V015 job-type list so there is one source of truth.

2. **`document_acl_state` is probably redundant.** A 1:1 table with a FK to
   `documents` adds a join and a write without adding information. Use a column,
   `documents.acl_version BIGINT NOT NULL DEFAULT 1` (and the
   `qdrant_synced_version BIGINT` from scenario B), mirroring how you already
   made `companies.acl_epoch` a column rather than a table. Bump it from the
   same triggers/functions that touch grants and the owner subtree.

3. **Tenant epoch vs. the existing company reverse-index overlap.** V008's
   `lr:eff:keys:company:{C}` already provides O(cached-entries) company-wide
   invalidation. `companies.acl_epoch` is a second mechanism for the same goal.
   Pick one as authoritative. I lean epoch (crash-safe monotonic compare, no
   DEL to lose) and demote the reverse-index Sets to a best-effort eager-evict
   optimization. State which one wins; today the doc has both without a
   tiebreak.

4. **Fail-open, not fail-closed, on overlay outage (2.2).** Under G1, a
   deny-list Redis outage should degrade to "all candidates fall through to the
   PG gate," not "tenant cannot search." Fail-closed is correct only if the
   deny-list is the boundary, which it must not be. Change 2.2's failure mode.

5. **As-of retrieval must bypass the live overlay (interaction with baseline 7
   and V024).** None of A-F consider as-of queries. The epoch, acl_version, and
   deny-list all reflect *now*; an as-of query authorizes against a *past*
   state. Resolve as-of authorization from the V024 `*_history` tables and
   ignore the live overlay entirely, otherwise a current deny-list entry could
   wrongly deny a historical read (recall bug) or, if the overlay were ever
   treated as authoritative, wrongly allow one (leak). State: *as-of reads
   resolve both axes temporally from V024 and do not consult the live epoch /
   version / deny-list.*

6. **`kSchemaVersion` 2 -> 3 implies a backfill prerequisite.** PR #24 just
   moved 1 -> 2 and there is still no resync worker. Old Qdrant points have no
   `acl_epoch` / `acl_version` keys, so glaze deserializes them to 0, and the
   gate will treat the entire pre-existing corpus as stale (version 0 < current
   1) until a one-time backfill runs. That is safe under G1 (every old chunk
   simply falls through to the PG gate) but it means the whole corpus runs in
   PG-fallback mode until backfill completes - a load concern for large
   tenants, not a correctness one. Call out the backfill migration as an
   explicit prerequisite and note the transient full-fallback period.

7. **Deny-list as per-document, not per-chunk, where possible.** A subtree move
   (2.5, W2) `SADD`s every affected chunk_id. For a 10k-document subtree that
   is potentially hundreds of thousands of set members written synchronously in
   the move transaction. A per-document deny marker (`lr:acl:deny:doc:{C}`
   holding doc_ids) collapses that to 10k entries and makes the gate's reject
   check `doc_id IN deny` (the gate already knows each candidate's
   `owner_org_unit_id` / document via the payload). Per-chunk granularity only
   matters if single chunks are revoked independently of their document, which
   the V008 model does not currently allow.

---

## 5. Acceptance criteria (Section 5) - test the boundary, not the cache

1. **The property test (5.3 / 2.5 invariant) is aimed at the wrong invariant.**
   The 2.5 invariant ("at no point is a chunk simultaneously not-denied AND
   version >= current AND granted-by-old-not-new") is a property of the
   *overlay*. The property that matters, and the one a security reviewer will
   ask for, is the Section 3 / G1 invariant: *for every interleaving of
   W1/W2/W3, the set of chunks the gate emits to a reader is always a subset of
   the authoritative-PG-allowed set at read time.* Write the property test as:
   fuzz random interleavings against a reference oracle (the PG truth), assert
   `gate_output is subset of oracle_allowed`, and separately assert recall
   recovers to equality once the outbox drains. This tests the wall, and it
   keeps passing even if you later simplify or remove the overlay.

2. **Add a test for scenario F explicitly:** membership revoke with a forced
   `lr:eff` DEL failure (mock Redis drop) must not return HR chunks to the
   removed user. If you adopt the epoch-covers-membership fix, this becomes a
   test that an un-bumped or lost reverse-index DEL is backstopped by the epoch.

3. **Add a test for scenario E's torn read:** concurrent `move_org_unit` during
   a scope re-resolution must yield either the pre-move or post-move scope,
   never a mix. Easiest to assert against the single-statement resolver.

---

## Summary of recommended changes

- Reframe all of Section 2 as recall/performance optimization; declare **G1**
  (gate authority) and **G2** (same-transaction epoch bump) as the only
  correctness load-bearing invariants.
- A, C, D: not leaks; cheap fixes (G1 for A; bounded SREM for C; TTL-on-deny
  for D, drop the sweeper).
- B: the guard cannot be transactional with Qdrant; serialize per-document and
  use `documents.qdrant_synced_version` as the Postgres-side compare-and-set.
- E: resolve scope in a single statement (or REPEATABLE READ) to kill the torn
  read; G2 makes the revoke-visibility free.
- F (new, most important): the reader axis (`lr:eff`) is unprotected against a
  lost membership-revoke invalidation. Extend the epoch to cover membership
  changes, or make the gate re-resolve the reader axis from PG.
- Schema: fold into V008/V015, replace `document_acl_state` with a `documents`
  column, pick epoch-vs-reverse-index as authoritative, fail-open on overlay
  outage, bypass overlay for as-of (V024), plan the schema-v3 backfill,
  consider per-document deny granularity.
- Acceptance: test the G1 subset property and scenarios E and F, not the
  overlay's internal invariant.

The design is close. The work is mostly in moving the correctness weight off
the overlay and onto the PG gate that Section 1 already promised, then writing
down the two invariants (G1, G2) that make the overlay safe to be as stale as
it likes.
