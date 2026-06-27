# Wikore Schema Review V2 — Feedback

Reviewer context: re-read full schema after Sonnet's two follow-up commits (`387f511` foundation pass, `d400e22` recommended/optional pass). Cross-referenced against the original `review_feedback.md` findings F1–F12.

**Headline: nothing in V010–V012 is a blocker. Ship it.** All twelve original findings are properly addressed or explicitly deferred. The V012 move_org_unit pattern (session-local GUC instead of `ALTER TABLE DISABLE TRIGGER`) is a genuinely better solution than what I sketched in the first review — well done. The V010 promotion function with `FOR UPDATE` + deprecate-first ordering + `COALESCE` for idempotency is textbook-correct.

What follows are some new, smaller findings introduced by the fixes (none blocking) plus a handful of polish items that emerged when re-reading with fresh eyes.

> **Update — applied in this same commit batch:** V2-N1, V2-N2, V2-N3, V2-N4, V2-N5, V2-N6, V2-N8 are all addressed in V013 + edits to V008/V010/V011/V012. New smoke tests 18–22 cover the no-op move guard, archived-promotion rejection, and the three reactivate paths. All 22 smoke tests pass. V2-N7 (concurrency negative tests) and V2-N9 (cardinality at scale) are deferred — operational, not pre-launch blockers.

---

## Original findings status

| # | Severity | Topic | Status |
|---|---|---|---|
| F1 | BLOCKER | Version-swap atomicity | ✅ Fixed via `promote_document_version()` |
| F2 | BLOCKER | Wiki page versioning | ✅ Fixed — `wiki_page_versions` mirrors documents, `wiki_page_sources` references version_id |
| F3 | RECOMMENDED | `users.deactivated_at` | ✅ Added, with two partial indexes for active-user fast paths |
| F4 | RECOMMENDED | `lr:eff` invalidation pattern | ✅ Reverse-index Sets replace SCAN/KEYS (one residual issue, see V2-N1 below) |
| F5 | RECOMMENDED | `memberships.expires_at` | ✅ Added with same CHECK + TTL-clamp policy |
| F6 | RECOMMENDED | `move_org_unit()` | ✅ Implemented; session-flag pattern is better than my original sketch |
| F7 | RECOMMENDED | Lifecycle timestamp trigger | ✅ Added as `set_document_version_lifecycle_timestamps()` |
| F8 | RECOMMENDED | Audit log index company prefix | ✅ Rebuilt |
| F9 | OPTIONAL | `chat_turns.rag_sources` JSONB shape | ✅ Lightweight `jsonb_typeof = 'array'` CHECK |
| F10 | OPTIONAL | PostgreSQL RLS | ⏸ Not addressed — was OPTIONAL, fine to defer |
| F11 | OPTIONAL | Storage quota tracking | ⏸ Not addressed — was OPTIONAL, fine to defer |
| F12 | OPTIONAL | Closure bulk-insert ordering note | ✅ Added to V001 comment |

---

## V2 findings (new, smaller items)

| # | Severity | Topic |
|---|---|---|
| V2-N1 | **RECOMMENDED** | Reverse-index Set TTL is not extended on subsequent `SADD`s — subtle invalidation hole |
| V2-N2 | **RECOMMENDED** | Re-granting an expired membership doesn't work as a simple INSERT (existing unique index doesn't consider expires_at) |
| V2-N3 | **RECOMMENDED** | User reactivation path isn't documented (deactivated user with same `external_sub` cannot rejoin via plain INSERT) |
| V2-N4 | **RECOMMENDED** | `move_org_unit()` vs concurrent descendant delete: races to an FK violation; document the retry contract |
| V2-N5 | OPTIONAL | `promote_document_version()` will silently promote an `archived` version back to `active` — intended or not? |
| V2-N6 | OPTIONAL | `move_org_unit()` no-op move (new_parent == current parent) does a full closure rebuild — cheap early-exit |
| V2-N7 | OPTIONAL | Smoke tests cover happy paths well; missing concurrency / negative cases |
| V2-N8 | OPTIONAL | `chat_turns_rag_sources_array_chk` has a dead `IS NULL` branch (column is `NOT NULL DEFAULT '[]'`) |
| V2-N9 | OPTIONAL | Reverse-index Set membership cardinality on very large companies |

---

## V2-N1. Reverse-index Set TTL hole (RECOMMENDED)

V008 says:
> "Set TTL for reverse-index keys should be longer than the eff TTL (30-60 min vs 5 min) so the Set outlives its members."

The hole: TTL is set once on first `SADD`. Subsequent `SADD`s do not refresh TTL. So a user whose cache entries keep getting refreshed every 5 minutes hits this sequence:
1. T=0: First request → `SET lr:eff:...` + `SADD lr:eff:keys:user:...` + (implicit) `EXPIRE lr:eff:keys:user:... 1800`
2. T=5..25: Refreshes every 5 min — only `SET` + `SADD`, TTL on the Set doesn't reset.
3. T=30: The Set's TTL fires. Set is gone. Cache entries are still alive.
4. T=31: Admin revokes a grant. Invalidation does `SMEMBERS lr:eff:keys:user:...` → empty. No DEL. Stale cache entries remain in `lr:eff:...` until their own 5-min TTL expires (so worst-case 5 min of stale data).

That 5-min stale window is the same as the eff TTL itself, so the invalidation has *effectively done nothing* on this user. The system is correct in the eventual-consistency sense (stale data eventually expires), but the invalidation operation silently fails its purpose for that user.

**Fix: refresh the Set TTL on every `SADD`.** Two writes per cache hit, both O(1):
```text
SET lr:eff:{cid}:{uid}:{oid} <value> EX 300
SADD lr:eff:keys:user:{cid}:{uid} "lr:eff:{cid}:{uid}:{oid}"
EXPIRE lr:eff:keys:user:{cid}:{uid} 1800
SADD lr:eff:keys:company:{cid} "lr:eff:{cid}:{uid}:{oid}"
EXPIRE lr:eff:keys:company:{cid} 1800
```

Or use a single Lua script for atomicity. Document the contract in V008.

Alternative: don't TTL the reverse-index Sets at all (live forever, get cleaned during normal DEL flow). Trade-off: Sets accumulate dead members over time but stay small (membership lifetime bounded by user activity). Either approach is fine; current spec is the only one with a real correctness issue.

---

## V2-N2. Re-granting an expired membership (RECOMMENDED)

V011 adds `memberships.expires_at` but the existing unique indexes from V002 don't consider it:
```sql
CREATE UNIQUE INDEX memberships_user_org_uidx
    ON memberships (company_id, user_id, org_unit_id)
    WHERE user_id IS NOT NULL;
```

Workflow problem: Admin grants Alice membership in HR until 2026-12-31. On 2027-01-15, admin wants to grant Alice membership in HR again for Q1 2027.

Natural admin intent: `INSERT INTO memberships ...` → fails with unique violation because the old (expired) row is still there.

Actual required workflow:
- `DELETE` the old row, then `INSERT` the new, **OR**
- `UPDATE` the old row's `expires_at` and `granted_at`

Both work, but the application has to know to do them — and the audit trail differs:
- DELETE+INSERT: two events (revoke, grant) — clear history but two audit log entries
- UPDATE: one event (extend) — concise but loses "when did the lapsed access actually end"

**Recommendations:**
- Document the chosen workflow in V011 (or in a comment near the unique index in V002).
- If "always one row per (user, org_unit)" is the desired invariant, add a comment explaining this. The current index implicitly enforces it.
- If you want expired rows to peacefully coexist (and the resolver to filter by `expires_at > now()`), change the unique index to be partial: `WHERE user_id IS NOT NULL AND (expires_at IS NULL OR expires_at > now())`. **Don't do this without thought** — `now()` in a partial index predicate is not immutable, PostgreSQL will reject it. You'd need a STABLE function wrapper, and even then the index becomes a maintenance burden.

The simplest fix is to document the DELETE+INSERT or UPDATE pattern as the canonical re-grant flow.

---

## V2-N3. User reactivation path (RECOMMENDED)

V011 comment:
> "The UNIQUE(company_id, external_issuer, external_sub) constraint intentionally stays in place so a deactivated identity cannot be silently recycled as a new user row unless the constraint is explicitly dropped first."

This protects against accidental identity recycling — good. But what's the **intended** reactivation flow when Alice (deactivated last quarter) rejoins on her existing SSO identity?

Naive admin attempt: `INSERT INTO users (...) VALUES (..., 'alice-sub-from-keycloak', ...)` → unique violation.

Required: `UPDATE users SET deactivated_at = NULL WHERE id = '<alice-old-id>'`

If the admin's path through the API is "POST /users with Alice's claim" without the API knowing about the old row, that fails. The application has to:
1. Check for an existing user with `(external_issuer, external_sub)` and `deactivated_at IS NOT NULL`
2. If found, reactivate (UPDATE deactivated_at = NULL)
3. If not found, insert

**Recommendation:** add a `reactivate_user(p_user_id)` function similar to `promote_document_version`:
```sql
CREATE OR REPLACE FUNCTION reactivate_user(p_user_id UUID)
RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    UPDATE users
    SET deactivated_at = NULL,
        last_seen      = NULL
    WHERE id = p_user_id AND deactivated_at IS NOT NULL;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'user % not found or not deactivated', p_user_id;
    END IF;
END;
$$;
```

And document the reactivate-vs-create decision tree in V011's header comment. Otherwise this will be re-discovered every time someone reads the schema.

---

## V2-N4. `move_org_unit()` vs concurrent descendant delete (RECOMMENDED)

`move_org_unit()` locks the moved node (`FOR UPDATE`) and the new parent. It does **not** lock the descendants. If a concurrent transaction `DELETE`s a descendant of the moved node mid-move:

1. Move tx reads the closure to compute the cross-join (snapshot includes descendant D).
2. Concurrent tx commits `DELETE FROM org_units WHERE id = D`. CASCADE removes D's closure rows.
3. Move tx Step 2: `INSERT INTO org_unit_closure ... SELECT ... d.descendant_id = D`. The composite FK `closure_descendant_same_company_fk REFERENCES org_units(company_id, id)` fails because D no longer exists.

The move tx aborts cleanly with an FK violation. **No data corruption** — but the caller needs to know to retry.

**Recommendation:** document the contract in the function header comment:
```sql
-- Concurrency: this function locks only the moved node and the new parent.
-- If a descendant of the moved subtree is deleted concurrently, the closure
-- INSERT in Step 2 will fail with a foreign-key violation. Callers MUST
-- handle the FK error and retry (or alternatively, take a SHARE-mode lock
-- on org_units before calling this function).
```

Optionally, for very-high-correctness deployments, add `LOCK TABLE org_units IN SHARE MODE;` at the top of the function. That blocks concurrent deletes during the move. Heavier (blocks ALL writes to org_units across the company while one move runs) but eliminates the race. Trade-off is yours.

---

## V2-N5. Promoting an archived version (OPTIONAL)

`promote_document_version()` checks `ingest_status = 'done'` but doesn't check `lifecycle_status`. So:
- An archived version (presumably long-deleted-from-Qdrant) can be promoted back to 'active'.
- The CHECK constraints are satisfied (active requires `ingest_status='done' AND completed_at NOT NULL AND activated_at NOT NULL AND superseded_at IS NULL` — the function sets `superseded_at = NULL` so this passes even on an archived row whose `superseded_at` was set).

Is this intended? Two interpretations:

**Intentional (rollback feature):** Allowing the admin to roll back to an old version after discovering a regression in the latest one. Then this is correct and worth a comment in V010: "any non-archived... wait, including archived".

**Unintentional:** Archived versions should be terminal. Then add `AND lifecycle_status <> 'archived'` to the version-validity check:
```sql
WHERE company_id    = p_company_id
  AND document_id   = p_document_id
  AND id            = p_version_id
  AND ingest_status = 'done'
  AND lifecycle_status <> 'archived';
```

Pick one, document the choice in V010.

---

## V2-N6. `move_org_unit()` no-op move (OPTIONAL)

Moving a node under its current parent does a full closure rebuild that produces the same set of rows it just deleted. Wasteful but correct. Cheap early-exit:

```sql
-- After the cycle check, before Step 1:
IF (SELECT parent_id FROM org_units
    WHERE company_id = p_company_id AND id = p_node_id) = p_new_parent_id
THEN
    RETURN;  -- no-op
END IF;
```

Saves work on the (admittedly rare) case where admin tooling accidentally calls move with the current parent.

---

## V2-N7. Smoke test coverage gaps (OPTIONAL)

Tests 11–17 cover the happy paths well. Worth adding:

- **Test 18**: `promote_document_version()` with `ingest_status='pending'` → expect "not promotable" error.
- **Test 19**: `promote_document_version()` with a version_id belonging to a different document → expect "not promotable" error.
- **Test 20**: Concurrent `promote_document_version()` for same document from two psql sessions → one succeeds, the other sees the first's outcome and either no-ops or also succeeds without leaving two active rows (depending on chosen semantics; lock should serialize them cleanly).
- **Test 21**: `move_org_unit()` with `new_parent_id` = a descendant of the moved node → expect "cycle" error.
- **Test 22**: `move_org_unit()` of root → expect "cannot move root" error.
- **Test 23**: `move_org_unit()` to a node in a different company → expect "new parent not found in company" error (cross-company guard).
- **Test 24**: Membership query with `WHERE expires_at IS NULL OR expires_at > now()` excludes the expired row. (Validates that the application's "active membership" pattern works against the new column.)

None blocking; nice for confidence at the next refactor.

---

## V2-N8. Dead CHECK branch (OPTIONAL)

```sql
ALTER TABLE chat_turns
    ADD CONSTRAINT chat_turns_rag_sources_array_chk
    CHECK (rag_sources IS NULL OR jsonb_typeof(rag_sources) = 'array');
```

`chat_turns.rag_sources` is `NOT NULL DEFAULT '[]'` (from V005), so `rag_sources IS NULL` is never true. The `IS NULL OR` branch is dead.

Harmless redundancy — looks like Sonnet copied a defensive pattern. Simplify to:
```sql
CHECK (jsonb_typeof(rag_sources) = 'array')
```

Same for `chat_turns_tool_calls_array_chk` (also `NOT NULL DEFAULT '[]'`).

---

## V2-N9. Reverse-index Set cardinality at scale (OPTIONAL)

`lr:eff:keys:company:{cid}` could grow to (users × scopes-per-user) members. For a 10k-user company with avg 5 cached scopes = 50k Set members. `SMEMBERS` on 50k strings returns ~3 MB of data and blocks the Redis event loop for ~10 ms. Not a problem for normal operation (membership invalidation is per-user, small). Becomes a problem on company-wide invalidation (org_unit delete) which is rare but does happen.

**Recommendations** (any one is fine):
- Use `SSCAN` instead of `SMEMBERS` for the company-wide Set on invalidation. Iterates in chunks. Acceptable since the operation is rare and not latency-sensitive.
- Shard the company Set: `lr:eff:keys:company:{cid}:{shard}` where `shard = hash(user_id) % 16`. Costs 16x more writes but each Set is bounded to 1/16 of total.
- Don't maintain `lr:eff:keys:company:*` at all. On company-wide invalidation, iterate through `lr:eff:keys:user:{cid}:*` instead — but that requires SCAN, which we just removed. So this option only works if you have an explicit per-company user list (which you probably do — `users.company_id` indexed).

For MVP, the simplest fix is: on company-wide invalidation, query Postgres `SELECT id FROM users WHERE company_id = $1`, then for each user_id do the per-user SMEMBERS+DEL. Skip the per-company Set entirely. The per-company Set is a premature optimization; per-user Set + Postgres user list is sufficient.

Not blocking; revisit if real customers complain about company-wide invalidation latency.

---

## Things I deliberately *don't* flag

- **`set_document_version_lifecycle_timestamps()` runs BEFORE `OF lifecycle_status` UPDATE.** I traced through the interaction with `promote_document_version()` — the SQL function sets both timestamps explicitly with COALESCE, the trigger then re-COALESCEs them (no-op), and the partial unique index is satisfied because deprecate-first ordering ensures the constraint sees at most one active row at any commit-able snapshot. Subtle but correct.
- **`clock_timestamp()` vs `now()` in V010.** `clock_timestamp()` is non-deterministic across rows in the same transaction. Intentional choice for accurate per-row timing; explicit in the function. ✓
- **`SECURITY DEFINER` not used on `move_org_unit`.** Correct — session-flag pattern avoids the need. Don't add it.
- **`set_config(..., is_local=true)` scoping.** Verified: scopes to the current transaction, auto-clears on commit or rollback. Cannot leak across transactions on the same connection. ✓
- **`wiki_page_versions` mirroring document_versions but missing some fields** (no `source_hash`, no `source_uri`, no `ingest_status`). Correct — wiki pages aren't ingested files, they're synthesized content. The omitted columns are document-specific. ✓
- **`wiki_page_sources` schema migration**: changed FK target from `wiki_page_id` to `wiki_page_version_id`. This is a breaking schema change but no data exists yet, so it's fine. Worth noting in the V004 header that this changed from the previous draft. (Or just rely on it being pre-MVP.)

---

## Top-3 to address before C++ work starts

All three are RECOMMENDED, not BLOCKER. The schema is ready for C++ as-is; these are pre-launch polish:

1. **V2-N1** — extend reverse-index Set TTL on every SADD. The 4 extra lines in the Redis client wrapper prevent silently broken invalidation.
2. **V2-N3** — write `reactivate_user()` and document the rejoin flow. This will be discovered the first time a user actually leaves and rejoins; better to discover it now.
3. **V2-N2** — pick a re-grant-expired-membership policy and document it. The application's first contractor renewal will surface this otherwise.

V2-N4 through V2-N9 can land as a polish PR any time before launch.

---

## Overall verdict

The schema went from "good MVP draft with two blockers" to "ready to write C++ against" in two commits. The blocker fixes are correct and the patterns chosen (session-flag GUC for move, FOR UPDATE for promotion, reverse-index Sets for invalidation) are all best-in-class for their problem class — not "good enough for MVP" choices.

The remaining items in this review are real but small. Ship.

— Reviewer: Claude (Opus 4.7)
