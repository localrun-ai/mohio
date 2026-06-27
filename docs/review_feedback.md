# Wikore Schema Review — Feedback

Reviewer context: read V001-V009 in full, cross-referenced with `docs/review.md` and `docs/feedback.md`. Findings tagged **BLOCKER** (data loss / correctness / breaks a stated invariant), **RECOMMENDED** (real-world ops or quality issue, worth fixing pre-launch), **OPTIONAL** (nice-to-have, can defer).

The schema is unusually well thought-through for an MVP. The composite-FK pattern is doing real work, the closure-table approach is the right call for the access-resolver hot path, and the two-axis Redis invalidation model in V008 is the single best piece of design in the package — that mental model alone will pay for itself.

What follows is the list of things I'd want fixed before any C++ code starts depending on the contract.

---

## Findings summary

| # | Severity | Topic |
|---|---|---|
| F1 | **BLOCKER** | `document_versions` active swap can't be atomic — partial unique index isn't deferrable |
| F2 | **BLOCKER** | Wiki page citation reproducibility — no `wiki_page_versions`, breaks the evidence-pinning claim if wikis are RAG inputs |
| F3 | **RECOMMENDED** | `users` needs `deactivated_at` for audit attributability |
| F4 | **RECOMMENDED** | `lr:eff` invalidation via `SCAN`/`KEYS pattern` is a production hazard at scale; use a per-user Set or Hash |
| F5 | **RECOMMENDED** | `memberships` should support `expires_at` (and feed the same TTL-clamp policy) |
| F6 | **RECOMMENDED** | `move_org_unit()` is referenced but undefined; the explicit no-op trigger makes moves currently impossible |
| F7 | **RECOMMENDED** | `document_versions.activated_at` / `superseded_at` should be set by trigger on lifecycle transition, not left to the application |
| F8 | **RECOMMENDED** | `audit_log_action_idx` missing `company_id` — cross-tenant scans for action-filtered queries |
| F9 | OPTIONAL | Wiki/version interaction with `chat_turns.rag_sources` JSONB shape |
| F10 | OPTIONAL | Add RLS as defence-in-depth |
| F11 | OPTIONAL | Storage quota tracking |
| F12 | OPTIONAL | Closure-table bulk-insert ordering caveat |

---

## Section 1 — Correctness and gaps

### F1. `document_versions` lifecycle swap is not atomic (BLOCKER)

V003 declares:
```sql
CREATE UNIQUE INDEX document_versions_one_active_per_doc_uidx
    ON document_versions (company_id, document_id)
    WHERE lifecycle_status = 'active';
```

This is correct in steady state but creates a hard problem at version promotion. The natural transaction is:
```sql
BEGIN;
  UPDATE document_versions SET lifecycle_status='deprecated', superseded_at=now()
    WHERE id = :old_active;
  UPDATE document_versions SET lifecycle_status='active', activated_at=now()
    WHERE id = :new_active;
COMMIT;
```

In PostgreSQL **partial UNIQUE indexes are not deferrable** (only UNIQUE constraints declared with `CONSTRAINT ... UNIQUE` can be `DEFERRABLE INITIALLY DEFERRED`, and only with EXCLUDE/UNIQUE constraints, not on the index form you used here). So the order matters:

- **Deprecate-first-then-activate** (as above) works.
- **Activate-first-then-deprecate** raises a unique-violation mid-transaction.

This is a latent footgun for the application code that nobody will hit until they write a "promote and rollback if anything fails" handler that does the activation first. The constraint silently disagrees with the natural-reading code.

**Fix options, in preference order:**

1. **Trigger-driven swap.** Replace the application-controlled transition with a `promote_document_version(version_id)` function that does both UPDATEs in the safe order, plus sets timestamps. Application calls one function; can't get it wrong.
2. **Re-declare as a deferrable EXCLUDE constraint:**
   ```sql
   ALTER TABLE document_versions
       ADD CONSTRAINT one_active_per_doc EXCLUDE
           (company_id WITH =, document_id WITH =) WHERE (lifecycle_status = 'active')
       DEFERRABLE INITIALLY DEFERRED;
   ```
   Then both orders work. Slightly slower checks but enables clean transactional swap.
3. Document the constraint loudly and add a regression test. Acceptable but I'd still prefer option 1.

---

### Answer to the explicit `lr:eff` key-structure question

The structure is **conceptually correct** but the invalidation pattern is what needs scrutiny, not the key shape.

The third dimension (`org_unit_id` = scope being queried from) is justified: a query "from within HR" should resolve to a subset that respects the session's chat scope, and that subset depends on the session context. So the cache key needs all three.

**Cardinality is fine.** Worst case is `users × sessions-per-user-different-org_units`, not `users × org_units_in_company`. In practice users have 1–10 active session scopes, not hundreds. Memory is bounded.

**Invalidation is the problem (F4 below).** Both `lr:eff:{company_id}:{user_id}:*` and `lr:eff:{company_id}:*` (used for grant changes) require `KEYS` or `SCAN`. Redis `KEYS` is banned in production guidance and `SCAN` has cursor lifetime issues and O(N over keyspace) cost. At 10k users × 5 scopes each = 50k keys per company, a `SCAN` on grant change will hit the worst cases.

### F4. Redis invalidation pattern (RECOMMENDED)

Replace the wildcard-key approach with **reverse-index sets**:

```text
lr:eff:{company_id}:{user_id}:{org_unit_id}     # the cached value, unchanged
lr:eff:users:{company_id}                       # Set of user_ids with any cache entry
lr:eff:keys:{company_id}:{user_id}              # Set of org_unit_ids cached for this user
```

On cache write: add user_id to `lr:eff:users:{company_id}` and org_unit_id to `lr:eff:keys:{company_id}:{user_id}`. On invalidation:

| Event | Strategy |
|---|---|
| Membership change for user U | `SMEMBERS lr:eff:keys:{company_id}:{U}` → DEL each `lr:eff:...:{U}:{ou}` → DEL the set |
| Group member add/remove | Same, scoped to the affected users |
| Resource grant create/revoke | `SMEMBERS lr:eff:users:{company_id}` → for each user, the per-user pattern above |

All O(actual cached entries), no `SCAN`, no keyspace walk. Maybe slightly more code but it's the production-correct version.

(Alternative: use a Redis Hash `lr:eff:{company_id}:{user_id}` with field=`org_unit_id`. Then `HDEL` is bulk-keyable and `DEL` clears the whole user in one op. Trade-off: lose per-field TTL, which the grant-expiry-clamp policy needs. Sets-plus-strings is the right shape here.)

### Answer to the `memberships.expires_at` question (F5, RECOMMENDED)

**Yes — add it, with the same TTL-clamp policy already documented for resource_grants.**

Real cases:
- Contractor with a 3-month engagement → membership expires when contract ends.
- Secondment: engineer temporarily added to Finance for a Q4 project.
- Break-glass access: incident responder added to Security for 24h.

The cost is small:
```sql
ALTER TABLE memberships ADD COLUMN expires_at TIMESTAMPTZ
    CHECK (expires_at IS NULL OR expires_at > granted_at);
```
Plus extend the `lr:eff` TTL clamp in V008 to also consider `MIN(m.expires_at)` from memberships.

Without this, the only way to give time-limited access is `resource_grants`, which is `org_unit`-scoped only (MVP constraint). Customer admins will work around it by setting a calendar reminder and manually deleting the row — which gives you zero audit trail of "when did access lapse vs when did the admin remember".

---

### Other Section 1 findings

**Trigger interaction note** — `org_unit_closure_on_insert` assumes `parent_id`'s closure rows already exist. For raw `INSERT INTO org_units VALUES (...), (...)` with parent and child in one statement, PostgreSQL processes in declared order, so it works if you write parent first. For bulk loads from CSV, the natural ordering is by `depth`. Add a one-liner to the V001 comment block noting "bulk inserts must be ordered such that any row's parent_id is inserted before the row itself". (F12, OPTIONAL.)

**`api_keys` cross-tenant lookup** is by design (`key_hash UNIQUE` globally), and the C++ validator necessarily queries without a `company_id` filter. The composite FK on `(company_id, user_id)` still enforces tenancy at write time. This is fine but should be called out in the V002 header so it doesn't look like a copy-paste miss.

---

## Section 2 — Hard-to-reverse decisions

### Migration cost: `access_scope_ids UUID[]` → `access_tokens TEXT[]`

This is the dominant lock-in. Rough cost estimate at the time you'd want to do it (say 100 customers, average 50k chunks each, so ~5M Qdrant points):

| Step | Cost |
|---|---|
| Postgres: `ALTER TABLE document_chunks ADD COLUMN access_tokens TEXT[]` | Trivial. |
| Postgres: backfill `access_tokens` from existing `access_scope_ids` + new principal grants | Batched UPDATE per company; ~minutes per company. |
| Qdrant: rewrite payload on every existing point | **This is the expensive part.** ~5M `set_payload` calls. At 200 req/s per Qdrant node ≈ 7 hours per node, longer if you respect cluster headroom. Online migration with dual-read during cutover is mandatory; offline window is unacceptable. |
| Application: dual-read code path that intersects on both columns during cutover | ~1 sprint of code + a thorough test pass. |
| Schema: `DROP COLUMN access_scope_ids` post-cutover | Trivial. |

**Recommendation to reduce future cost**: declare `access_tokens TEXT[]` **now** as a nullable second column on `document_chunks`, even though no code reads it for MVP. The Qdrant payload doesn't need it yet. When you cut over, you batch-populate the column, then the resync queue (which you already designed for grant changes — reuse it) rewrites Qdrant in the background, then a flag flip on the application side switches readers from `access_scope_ids` to `access_tokens`. The schema migration is then cosmetic.

The big win: you've already built the resync queue (V008 `lr:resync:q`), the access-scope recompute path, and the principal-of-changes ID propagation. The migration tooling is mostly the same. The remaining bottleneck is just the Qdrant rewrite throughput.

### Other hard-to-reverse items

- **`org_unit_closure` table choice** is correct and not particularly hard to reverse if you ever needed to (recursive CTE works). Don't worry about this one.
- **Composite FK pattern**: irreversible without a full schema rewrite, but I wouldn't reverse it. It's a strict improvement over the alternative.
- **`document_chunks.access_scope_ids UUID[]` as source of truth (not denormalized from grants)**: this is correct. The denormalization-to-Qdrant pattern is sound. Don't undo it.

---

## Section 3 — Missing tables / features

### F2. Wiki page versioning (BLOCKER if wikis are RAG inputs)

V004 stores wiki page content as a single mutable `TEXT` column. If wiki pages are ever fed into the RAG pipeline as evidence (and the framing in `docs/feedback.md` strongly suggests they will be — "wiki is a projection of the evidence layer"), then citation reproducibility is broken: `chat_turns.rag_sources` JSONB stores `version_id` for documents but there is no `version_id` for wiki pages because they aren't versioned.

If wiki pages are display-only (read by humans, never fed to the LLM), this isn't a blocker — but then the "evidence reproducibility" claim in V003's header comment is asymmetric and worth noting in the design.

**Two reasonable resolutions:**

A. **Bring wiki pages under the document_versions pattern.** Reuse the same lifecycle states and add `wiki_page_versions` mirroring `document_versions`. Cleanest semantically; ~80 LoC of schema.

B. **Snapshot-on-cite.** Each `wiki_page_sources` row captures the wiki content hash + activated_at at synthesis time. Older but unmodifiable. Cheaper if wiki edits are rare.

Pick one before the C++ wiki-as-RAG path exists. Retrofitting either is painful once `chat_turns.rag_sources` JSONB has unversioned wiki references in production data.

### F3. `users.deactivated_at` for audit integrity (RECOMMENDED)

The V007 audit-log header explicitly notes:
> "Callers MUST denormalize identity into detail at write time, e.g. `{ user_email: alice@acme.com }`"

This works at write time but breaks audit-log UI integrity at read time. Showing "audit_log row references user `<uuid>` (user not found)" is the kind of thing that fails compliance reviews. With a `deactivated_at` column on users, you can hard-delete the SSO link (`external_issuer`, `external_sub`, `email` nulled and replaced with a placeholder) while keeping the row for FK / audit reference. The "GDPR delete" use case can null the PII but keep the audit anchor.

Concretely:
```sql
ALTER TABLE users
    ADD COLUMN deactivated_at TIMESTAMPTZ;

-- Optional: prevent login by deactivated users (application or trigger).
CREATE INDEX users_active_idx ON users (company_id, id) WHERE deactivated_at IS NULL;
```

The application logic for "active users" becomes `deactivated_at IS NULL`. Cheap to add now, expensive to retrofit after audit reports start failing.

### F6. `move_org_unit()` undefined but blocked (RECOMMENDED)

`prevent_org_unit_parent_update` makes any parent change raise. There's no implementation of `move_org_unit()`. So today, **org units cannot be moved at all** — including the routine "Sales team moved from Marketing to Revenue" case. For an MVP this might be acceptable, but the documentation implies the function exists.

Sketch of a correct implementation, since the question explicitly asked about edge cases:

```sql
CREATE OR REPLACE FUNCTION move_org_unit(
    p_company_id  UUID,
    p_node_id     UUID,
    p_new_parent  UUID
) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    -- Edge case 1: cross-company move forbidden.
    IF NOT EXISTS (SELECT 1 FROM org_units
                   WHERE id = p_new_parent AND company_id = p_company_id) THEN
        RAISE EXCEPTION 'cross-company move not allowed';
    END IF;

    -- Edge case 2: moving root forbidden.
    IF EXISTS (SELECT 1 FROM org_units
               WHERE id = p_node_id AND type = 'root') THEN
        RAISE EXCEPTION 'cannot move root org_unit';
    END IF;

    -- Edge case 3: cycle prevention (new parent cannot be a descendant of node).
    IF EXISTS (SELECT 1 FROM org_unit_closure
               WHERE company_id = p_company_id
                 AND ancestor_id = p_node_id
                 AND descendant_id = p_new_parent) THEN
        RAISE EXCEPTION 'cycle: new parent is a descendant of the node';
    END IF;

    -- Closure surgery (inside a transaction; let the caller open one).
    -- 1. Delete closure rows that connect old ancestors to descendants of node.
    DELETE FROM org_unit_closure c
    WHERE c.company_id = p_company_id
      AND c.descendant_id IN (SELECT descendant_id FROM org_unit_closure
                              WHERE company_id = p_company_id
                                AND ancestor_id = p_node_id)
      AND c.ancestor_id NOT IN (SELECT descendant_id FROM org_unit_closure
                                WHERE company_id = p_company_id
                                  AND ancestor_id = p_node_id);

    -- 2. Insert closure rows linking new ancestors to all descendants of node.
    INSERT INTO org_unit_closure (company_id, ancestor_id, descendant_id, depth)
    SELECT p_company_id, super.ancestor_id, sub.descendant_id,
           super.depth + sub.depth + 1
    FROM org_unit_closure super
    CROSS JOIN org_unit_closure sub
    WHERE super.company_id = p_company_id
      AND super.descendant_id = p_new_parent
      AND sub.company_id = p_company_id
      AND sub.ancestor_id = p_node_id;

    -- 3. Update the parent pointer. Need to temporarily disable the trigger.
    ALTER TABLE org_units DISABLE TRIGGER org_units_prevent_parent_update;
    UPDATE org_units SET parent_id = p_new_parent WHERE id = p_node_id;
    ALTER TABLE org_units ENABLE TRIGGER org_units_prevent_parent_update;

    -- 4. Operational follow-up: enqueue Qdrant resync for descendants.
    -- (Application or trigger writes lr:resync:q entries here.)
END;
$$;
```

Three subtle edge cases worth flagging:
1. **`ALTER TABLE ... DISABLE TRIGGER` requires ownership or superuser.** The application role won't have it. Either grant the privilege, or use `SET session_replication_role = 'replica'` inside the function (similarly privileged), or use a dedicated function-owner role with `SECURITY DEFINER`.
2. **Cross-company move**: explicitly raise. Allowing it would break the composite FK guarantees on every dependent table (`documents.owner_org_unit_id`, `wiki_pages.org_unit_id`, etc.) — the FK enforces `(company_id, owner_org_unit_id)` matches the parent table's row. Cross-company move would have to relocate every dependent row too. Not in scope for any sane product.
3. **Moving root**: explicitly raise. The `org_units_one_root_per_company_uidx` and the `companies_create_root_org_unit` trigger together depend on root being immobile.

### Storage quotas (F11, OPTIONAL)

The `size_bytes` column exists; the aggregate doesn't. If you decide you need quotas, a materialized view per company is cheap:
```sql
CREATE MATERIALIZED VIEW company_storage_summary AS
SELECT company_id, SUM(size_bytes) AS bytes
FROM document_versions
WHERE lifecycle_status <> 'archived'
GROUP BY company_id;
```
Refresh on a schedule, or maintain via trigger. Not needed for MVP unless your billing model depends on it.

---

## Section 4 — Future extensibility

### Group hierarchy (nested groups)

Adding nested groups later is **cheap, not painful**:
1. Add `groups.parent_id UUID` (composite FK to `(company_id, id)`).
2. Add a `groups_closure` table modelled exactly on `org_unit_closure`.
3. Update the access resolver to expand groups recursively before computing membership.

Nothing in the current schema is hostile to this. The access resolution semantics don't change — groups still resolve to user sets, and user sets still match memberships. No data migration required other than the new tables.

### Re-embedding progress tracking

Out of scope for the DB. The data needed is already there:
```sql
SELECT dc.id FROM document_chunks dc
LEFT JOIN document_chunk_vectors v
    ON v.chunk_id = dc.id AND v.embedding_model_id = :new_model
WHERE v.chunk_id IS NULL;
```
That query finds every chunk not yet embedded with the new model. A worker can drive off that. If you want a UI for progress, materialize `(model_id, total, done, in_flight)` in Redis or a small jobs table.

### `companies.settings JSONB` guidance

What belongs in `settings`:
- Feature flags (`{"feature_wiki_v2": true, "feature_audit_export": false}`)
- Per-tenant rate-limit overrides (`{"rate_limits": {"chat_per_min": 60}}`)
- LLM model selection (`{"llm": {"model": "qwen3-8b", "endpoint": "..."}}`)
- UI preferences (`{"branding": {...}}`)

What does **not** belong:
- Secrets (use a dedicated secrets table or external Vault; `integrations.credentials` is the right pattern)
- Anything you'll ever want to query by (`WHERE settings->>'plan' = 'enterprise'`-style queries become un-indexable; promote to a column).

Validation: add a check trigger that validates the JSONB matches a known schema (see `pgjsonschema` extension if you want strict typing). Or document the expected keys and validate at the application boundary. The latter is fine for MVP.

---

## Section 5 — Operational

### F8. `audit_log_action_idx` missing `company_id` (RECOMMENDED)

```sql
CREATE INDEX audit_log_action_idx ON audit_log (action, created_at DESC);
```

Queries like "all logins in company X this week" become a cross-tenant scan. For SaaS-style deployments this leaks per-tenant query cost into other tenants' shared resources. Trivial fix:
```sql
CREATE INDEX audit_log_action_idx ON audit_log (company_id, action, created_at DESC);
```
(Drop the existing single-column-prefixed one if there's no query that uses it.)

### F7. `activated_at` / `superseded_at` should be trigger-driven (RECOMMENDED)

V003 has strong CHECK constraints requiring `activated_at IS NOT NULL` for active versions and `superseded_at IS NOT NULL` for deprecated-after-active versions. But the trigger never sets them — the application must remember to write the timestamps. This is a footgun: the constraints will catch the bug, but only at the worst time (production INSERT failure during a deploy).

Replace with a trigger:
```sql
CREATE OR REPLACE FUNCTION sync_lifecycle_timestamps()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF OLD.lifecycle_status IS DISTINCT FROM NEW.lifecycle_status THEN
        IF NEW.lifecycle_status = 'active' AND NEW.activated_at IS NULL THEN
            NEW.activated_at := now();
        END IF;
        IF NEW.lifecycle_status IN ('deprecated', 'archived')
           AND OLD.lifecycle_status = 'active'
           AND NEW.superseded_at IS NULL THEN
            NEW.superseded_at := now();
        END IF;
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER document_versions_lifecycle_timestamps
    BEFORE UPDATE OF lifecycle_status ON document_versions
    FOR EACH ROW EXECUTE FUNCTION sync_lifecycle_timestamps();
```

Application can still override (passes explicit values), but doesn't have to remember.

### Partition management

The DEFAULT partition is the right safety net. The full story needs more:
- **`pg_partman`** is the standard answer for PostgreSQL partition lifecycle. Schedules new partitions ahead of time, optionally detaches old ones for archival. Add to the deployment runbook.
- **What to do with old partitions**: detach + `pg_dump` to cold storage, then `DROP TABLE`. Audit logs are append-only and historical access is rare — keeping 7 years of detached partitions on cheap storage is the standard pattern.
- **Without pg_partman**: a cron job that runs:
  ```sql
  CREATE TABLE IF NOT EXISTS audit_log_2027_q3 PARTITION OF audit_log
      FOR VALUES FROM ('2027-07-01') TO ('2027-10-01');
  ```
  6 months ahead of time. Same idea, just hand-rolled.

### F10. PostgreSQL RLS as defence-in-depth (OPTIONAL)

For a self-hosted enterprise product where customers see the schema and the binary is deployed in their environment, RLS gives you **a real defence-in-depth story to put in the security review document**, separate from any actual bug it prevents.

Cost: small. Per table:
```sql
ALTER TABLE documents ENABLE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON documents
    USING (company_id = current_setting('app.current_company_id')::uuid);
```
Plus `SET LOCAL app.current_company_id = '...'` at the start of every transaction in C++.

The argument **against** is that it shifts the failure mode from "we tested every query handler" to "we tested every place that sets the GUC". You're not eliminating a class of bugs, you're moving them. For an enterprise customer with a security team that wants the RLS checkbox ticked, the move is worth it. For a small team that has tight test coverage on the application layer, debatable.

My take: do it pre-MVP. The cost is in one round-trip per transaction (negligible) and you get the auditors-happy property for free.

### V009 trigger hot path

Real but small. `resource_grants` writes are an admin operation, not a user operation. At 10 admin actions per minute across the entire deployment the lookup cost is invisible. If you ever hit the case where it matters, the answer is batch grant creation behind a single function that does the validation once for the batch.

---

## Section 6 — Anything else

### F9. `chat_turns.rag_sources` schema is unenforced (OPTIONAL)

```sql
rag_sources JSONB NOT NULL DEFAULT '[]',
-- [{doc_id, version_id, chunk_id, score, excerpt}]
```

This is the most important JSONB column in the system — it's the audit trail of "what evidence was shown to the user". A schema regression here corrupts citation reproducibility silently. Recommend either:
- A `CHECK` constraint enforcing array shape and key presence (simple but limited)
- A `jsonb_valid` UDF that validates against an explicit schema (more robust)
- A separate `chat_turn_sources` table with FKs (most correct but heaviest)

The lightest option that catches the common regressions:
```sql
CONSTRAINT chat_turns_rag_sources_is_array
    CHECK (jsonb_typeof(rag_sources) = 'array')
```

### `chat_sessions` membership check

`chat_sessions.org_unit_id` accepts any org_unit in the company even if the user has no membership there. This is fine because the access resolver enforces it at query time, but it's worth a one-line comment in V005 noting "no FK-level check that user_id is a member of org_unit_id; access resolution happens at chat_turn write time, not session creation". Otherwise the next person reading the schema will assume the constraint exists.

### `integrations.credentials_key_id` operational tooling

The schema supports key rotation; the actual rotation needs operator tooling. Specifically, a `key_id_in_use` query to find rows still using a retiring key. Trivial as a one-liner:
```sql
SELECT credentials_key_id, COUNT(*)
FROM integrations
WHERE credentials_key_id IS NOT NULL
GROUP BY credentials_key_id;
```
Document this in the ops runbook.

### `embedding_models` is global, not company-scoped

Stated as a finalized decision indirectly (no `company_id` on the table). For self-hosted single-tenant this is correct. For a SaaS deployment where company A wants `bge-m3` and company B wants a custom model, you'd add `company_id NULL` (NULL = global) and adjust the unique constraints. Trivial to migrate later. No action now.

---

## Things I deliberately did not flag

- **`resource_grants` polymorphic discriminator** — V009 trigger handles the integrity gap correctly. The trigger's per-row lookup cost is fine for grant write rates.
- **`api_keys.key_hash UNIQUE` global** — correct.
- **Composite FK pattern overhead** — yes it's verbose, but eliminating an entire class of cross-tenant data leak at the DB level is worth the verbosity 10x over. Don't change this.
- **No `updated_at` trigger on `chat_sessions` driven by `chat_turns` writes** — UX issue, not a correctness one. Application can `UPDATE chat_sessions SET updated_at = now() WHERE id = $session_id` in the same transaction as the turn insert.

---

## Top-3 to fix before the C++ work starts

1. **F1** — fix the version-swap atomicity (trigger or deferrable constraint). The application code will be much less prone to subtle bugs if the activation flow is one function call.
2. **F2** — decide the wiki versioning model now. The shape of `chat_turns.rag_sources` and `wiki_page_sources` will harden as soon as code starts writing rows.
3. **F4** — design the cache invalidation around per-user Sets / Hashes rather than wildcard SCAN. Adding it now is a few lines; adding it after the application has been written against the wildcard pattern is a coordinated change across cache-write and cache-invalidate paths.

Everything else can be follow-up PRs without painful migrations.

— Reviewer: Claude (Opus 4.7)
