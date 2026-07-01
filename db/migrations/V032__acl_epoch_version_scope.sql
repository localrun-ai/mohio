-- V032: ACL epoch / per-document version / per-user scope epoch + prefilter rename
--
-- Implements the schema and G2 bump-point wiring for Iteration 2's access
-- resolution layer, per docs/iteration_2_design.md v2.3.
--
-- What this migration adds:
--   1. companies.acl_epoch          - broad reader-scope invalidation signal.
--   2. documents.acl_version        - per-document payload-staleness signal.
--      documents.qdrant_synced_version - monotonic "latest version in Qdrant".
--   3. users.scope_epoch            - per-user reader-scope invalidation signal.
--   4. document_chunks.access_scope_ids RENAMED to qdrant_prefilter_scope_ids
--      (demoted to a Qdrant prefilter hint; the gate resolves the resource
--      axis live from resource_grants per G1/option (a), so the column is no
--      longer an access authority).
--   5. AFTER triggers that perform the bumps in the SAME transaction as the
--      authoritative change (G2), plus a break-glass epoch function and the
--      §2.7 backfill enqueue function.
--
-- ---------------------------------------------------------------------------
-- BUMP POLICY (note the cache each signal gates; they are different caches)
--
--   companies.acl_epoch  gates lr:eff (the reader's RESOLVED READ-SCOPE cache).
--     A reader's scope is their org_unit memberships expanded by closure. It
--     changes only when the org TREE STRUCTURE changes for many users at once:
--       * org_unit create / delete / move (parent_id change) - closure shifts.
--       * break-glass (operator-invoked mass invalidation).
--     It does NOT change on resource_grant or owner changes: those are
--     resource-axis events that never alter who a user is.
--
--   users.scope_epoch    gates lr:eff for ONE user (narrow reader-scope change):
--       * memberships insert/update/delete (the affected user, or every user
--         in the affected group).
--       * group_members insert/update/delete (the affected user).
--
--   documents.acl_version gates QDRANT PAYLOAD trust (resource axis):
--       * resource_grants insert/update/delete touching the document (directly,
--         or via an org_unit grant whose subtree covers the document's owner).
--       * documents.owner_org_unit_id change (reassignment).
--       * org_unit move fan-out (closure change re-points inherited grants) -
--         see the NOTE on moves below.
--
-- DEVIATION FROM v2.3 §2.1 (ratify or revert): the v2.3 bump-point list says
-- "companies.acl_epoch: Any resource_grants insert/update/delete" and lists
-- owner-change under acl_epoch too. That conflicts with (i) §2.1's own
-- "Why both" rationale (single-document grant changes should use the per-doc
-- version, NOT force every tenant read to invalidate) and (ii) the fact that,
-- under option (a), the gate resolves grants live and lr:eff holds only the
-- reader's memberships - which a grant change never touches. Bumping the
-- company epoch on every grant would stampede every tenant lr:eff for a
-- resource-axis event. This migration therefore bumps acl_epoch ONLY on org
-- structural changes + break-glass, and routes grants/owner changes to
-- acl_version. If you prefer the literal §2.1 text, move the
-- "UPDATE companies SET acl_epoch" line into wikore_grant_acl_bump().
--
-- NOTE on org_unit moves: the company-epoch bump below restores reader-scope
-- correctness for a move (every lr:eff re-resolves; the gate resolves live).
-- The per-document acl_version fan-out + deny-list SADD + per-doc resync
-- enqueue for the moved subtree (§2.5) is application-orchestrated in the move
-- use-case, not in this trigger, because the affected-document set is a closure
-- expansion and the deny-list is a Redis write. Correctness does not depend on
-- it (G1 + epoch hold the line); it is a recall optimization.
--
-- NOTE on the Redis deny-list: triggers cannot write Redis. The resync outbox
-- event enqueued here is the durable record; the wikore-scheduler worker SADDs
-- lr:acl:deny:{company} on claim and SREMs on completion (§2.2 / §2.3). Under
-- option (a) the deny-list is a pure recall optimization, so the commit-to-
-- claim gap is recall-only, never a leak.
--
-- COORDINATED C++ CHANGE REQUIRED: the column rename breaks the write path.
-- src/ingest/document_repo.cpp (INSERT ... access_scope_ids) and
-- src/scheduler/outbox_worker.cpp (array_to_string(dc.access_scope_ids,...))
-- must switch to qdrant_prefilter_scope_ids in the same change set.
-- ---------------------------------------------------------------------------

-- ===========================================================================
-- 1. Columns
-- ===========================================================================

ALTER TABLE companies
    ADD COLUMN acl_epoch BIGINT NOT NULL DEFAULT 1;

ALTER TABLE documents
    ADD COLUMN acl_version           BIGINT NOT NULL DEFAULT 1,
    ADD COLUMN qdrant_synced_version BIGINT NOT NULL DEFAULT 0;

ALTER TABLE users
    ADD COLUMN scope_epoch BIGINT NOT NULL DEFAULT 1;

-- ===========================================================================
-- 2. Demote + rename the denormalized chunk scope to an explicit prefilter key
--
-- Under G1/option (a) the gate resolves the resource axis live from
-- resource_grants; this column is only the Qdrant payload source
-- (outbox_worker.cpp) and the resync-targeting GIN index, never the boundary.
-- ===========================================================================

ALTER TABLE document_chunks
    RENAME COLUMN access_scope_ids TO qdrant_prefilter_scope_ids;

ALTER INDEX document_chunks_scopes_idx
    RENAME TO document_chunks_prefilter_scope_idx;

-- ===========================================================================
-- 3. Helper: bump acl_version for a set of documents and enqueue a per-document
--    resync outbox event carrying the NEW version. Idempotency key coalesces
--    repeated bumps for the same (document, version) into one event.
-- ===========================================================================

CREATE OR REPLACE FUNCTION wikore_bump_docs_and_enqueue(
    p_company_id   UUID,
    p_document_ids UUID[]
) RETURNS VOID
LANGUAGE plpgsql AS $$
BEGIN
    IF p_document_ids IS NULL OR array_length(p_document_ids, 1) IS NULL THEN
        RETURN;
    END IF;

    WITH bumped AS (
        UPDATE documents d
        SET    acl_version = d.acl_version + 1,
               updated_at  = now()
        WHERE  d.company_id = p_company_id
          AND  d.id = ANY(p_document_ids)
        RETURNING d.id, d.company_id, d.acl_version
    )
    INSERT INTO outbox_events
        (company_id, aggregate_id, job_type, job_schema_version,
         payload, idempotency_key)
    SELECT b.company_id, b.id, 'qdrant_resync_chunk_acl', 1,
           jsonb_build_object(
               'document_id', b.id,
               'acl_version', b.acl_version,
               'reason',      'acl_change'),
           'resync:' || b.id::text || ':' || b.acl_version::text
    FROM   bumped b
    ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING;
END;
$$;

-- ===========================================================================
-- 4. resource_grants -> documents.acl_version (+ resync enqueue). NO epoch bump.
--
-- Affected documents:
--   resource_type='document'  -> exactly resource_id.
--   resource_type='org_unit'  -> documents whose owner_org_unit_id is in the
--                                grant's resource subtree (self_only = depth 0;
--                                self_and_descendants = whole subtree).
--   resource_type='wiki_page' -> no documents (wiki has its own lifecycle).
--
-- Re-pointing a grant's resource is an unusual operation (the UNIQUE key is on
-- the resource identity); permission/expires_at updates keep the same resource,
-- so handling the effective (NEW-or-OLD) row is sufficient.
-- ===========================================================================

-- Documents a grant tuple covers: the exact document, or every document whose
-- owner is in the org_unit resource's subtree (self_only = depth 0).
CREATE OR REPLACE FUNCTION wikore_grant_affected_docs(
    p_company UUID, p_rtype TEXT, p_rid UUID, p_applies TEXT
) RETURNS UUID[]
LANGUAGE plpgsql AS $$
DECLARE v_docs UUID[];
BEGIN
    IF p_rtype = 'document' THEN
        v_docs := ARRAY[p_rid];
    ELSIF p_rtype = 'org_unit' THEN
        SELECT array_agg(d.id) INTO v_docs
        FROM   documents d
        WHERE  d.company_id = p_company
          AND  d.owner_org_unit_id IN (
              SELECT c.descendant_id
              FROM   org_unit_closure c
              WHERE  c.company_id  = p_company
                AND  c.ancestor_id = p_rid
                AND  (p_applies = 'self_and_descendants' OR c.depth = 0)
          );
    ELSE
        v_docs := NULL;   -- wiki_page: no documents
    END IF;
    RETURN v_docs;
END;
$$;

CREATE OR REPLACE FUNCTION wikore_grant_acl_bump()
RETURNS TRIGGER
LANGUAGE plpgsql AS $$
BEGIN
    -- Process the OLD and NEW grant scopes SEPARATELY, each under ITS OWN
    -- company_id. Narrowing (self_and_descendants -> self_only) or moving a
    -- grant must resync the documents it no longer covers, or their Qdrant
    -- payloads stay permanently stale. Keeping each side on its own tenant also
    -- means a cross-tenant change (should V009 ever permit one) resyncs
    -- documents in BOTH tenants instead of filtering the OLD tenant's out.
    IF TG_OP IN ('UPDATE','DELETE') THEN
        PERFORM wikore_bump_docs_and_enqueue(
            OLD.company_id,
            wikore_grant_affected_docs(OLD.company_id, OLD.resource_type,
                                       OLD.resource_id, OLD.resource_applies_to));
    END IF;
    IF TG_OP IN ('UPDATE','INSERT') THEN
        PERFORM wikore_bump_docs_and_enqueue(
            NEW.company_id,
            wikore_grant_affected_docs(NEW.company_id, NEW.resource_type,
                                       NEW.resource_id, NEW.resource_applies_to));
    END IF;
    RETURN NULL;
END;
$$;

CREATE TRIGGER resource_grants_acl_version_bump
    AFTER INSERT OR UPDATE OR DELETE ON resource_grants
    FOR EACH ROW EXECUTE FUNCTION wikore_grant_acl_bump();

-- ===========================================================================
-- 5. documents.owner_org_unit_id change -> that document's acl_version.
--    Resource axis (the doc's home OU changed). NO epoch bump. The WHEN clause
--    keeps the nested acl_version UPDATE in the helper from re-firing.
-- ===========================================================================

CREATE OR REPLACE FUNCTION wikore_document_owner_acl_bump()
RETURNS TRIGGER
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM wikore_bump_docs_and_enqueue(NEW.company_id, ARRAY[NEW.id]);
    RETURN NULL;
END;
$$;

CREATE TRIGGER documents_owner_change_acl_bump
    AFTER UPDATE OF owner_org_unit_id ON documents
    FOR EACH ROW
    WHEN (OLD.owner_org_unit_id IS DISTINCT FROM NEW.owner_org_unit_id)
    EXECUTE FUNCTION wikore_document_owner_acl_bump();

-- ===========================================================================
-- 6. org_units structural change -> companies.acl_epoch (broad reader-scope
--    invalidation). Create / delete / move (parent_id) all shift the closure
--    and therefore the resolved scope of s_and_d members.
-- ===========================================================================

CREATE OR REPLACE FUNCTION wikore_org_structure_epoch_bump()
RETURNS TRIGGER
LANGUAGE plpgsql AS $$
DECLARE
    c_id UUID := COALESCE(NEW.company_id, OLD.company_id);
BEGIN
    -- A re-parent UPDATE that does not actually change parent_id is not a
    -- structural change; skip the bump. (AFTER UPDATE OF parent_id fires on
    -- assignment even when the value is unchanged.) INSERT/DELETE always bump.
    IF TG_OP = 'UPDATE' AND NEW.parent_id IS NOT DISTINCT FROM OLD.parent_id THEN
        RETURN NULL;
    END IF;
    UPDATE companies SET acl_epoch = acl_epoch + 1 WHERE id = c_id;
    RETURN NULL;
END;
$$;

CREATE TRIGGER org_units_acl_epoch_bump
    AFTER INSERT OR DELETE OR UPDATE OF parent_id ON org_units
    FOR EACH ROW EXECUTE FUNCTION wikore_org_structure_epoch_bump();

-- ===========================================================================
-- 7. memberships -> users.scope_epoch (the user, or every user in the group).
-- ===========================================================================

-- Bump every user whose resolved scope depends on a membership's principal
-- (the user directly, or every member of the group).
CREATE OR REPLACE FUNCTION wikore_bump_membership_principal(
    p_company UUID, p_user UUID, p_group UUID
) RETURNS VOID
LANGUAGE plpgsql AS $$
BEGIN
    IF p_user IS NOT NULL THEN
        UPDATE users SET scope_epoch = scope_epoch + 1
        WHERE company_id = p_company AND id = p_user;
    ELSIF p_group IS NOT NULL THEN
        UPDATE users SET scope_epoch = scope_epoch + 1
        WHERE company_id = p_company
          AND id IN (SELECT gm.user_id FROM group_members gm
                     WHERE gm.company_id = p_company AND gm.group_id = p_group);
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION wikore_membership_scope_bump()
RETURNS TRIGGER
LANGUAGE plpgsql AS $$
BEGIN
    -- Bump BOTH the losing (OLD) and the gaining (NEW) principal. An UPDATE
    -- that reassigns a membership to a different user/group must invalidate the
    -- OLD principal's cache too, or that user keeps revoked access until the
    -- lr:eff TTL expires. Bumping the same user twice is harmless (the epoch
    -- only has to change).
    IF TG_OP IN ('UPDATE','DELETE') THEN
        PERFORM wikore_bump_membership_principal(OLD.company_id, OLD.user_id, OLD.group_id);
    END IF;
    IF TG_OP IN ('UPDATE','INSERT') THEN
        PERFORM wikore_bump_membership_principal(NEW.company_id, NEW.user_id, NEW.group_id);
    END IF;
    RETURN NULL;
END;
$$;

CREATE TRIGGER memberships_scope_epoch_bump
    AFTER INSERT OR UPDATE OR DELETE ON memberships
    FOR EACH ROW EXECUTE FUNCTION wikore_membership_scope_bump();

-- ===========================================================================
-- 8. group_members -> the affected user's scope_epoch (their group composition,
--    and therefore which org_unit memberships apply to them, changed).
-- ===========================================================================

CREATE OR REPLACE FUNCTION wikore_group_member_scope_bump()
RETURNS TRIGGER
LANGUAGE plpgsql AS $$
BEGIN
    -- Bump the losing (OLD) and the gaining (NEW) user; a reassigned row must
    -- invalidate both principals' caches.
    IF TG_OP IN ('UPDATE','DELETE') THEN
        UPDATE users SET scope_epoch = scope_epoch + 1
        WHERE company_id = OLD.company_id AND id = OLD.user_id;
    END IF;
    IF TG_OP IN ('UPDATE','INSERT') THEN
        UPDATE users SET scope_epoch = scope_epoch + 1
        WHERE company_id = NEW.company_id AND id = NEW.user_id;
    END IF;
    RETURN NULL;
END;
$$;

CREATE TRIGGER group_members_scope_epoch_bump
    AFTER INSERT OR UPDATE OR DELETE ON group_members
    FOR EACH ROW EXECUTE FUNCTION wikore_group_member_scope_bump();

-- ===========================================================================
-- 9. Break-glass: operator-invoked tenant-wide lr:eff invalidation.
--    Returns the new epoch so the caller can log it.
-- ===========================================================================

CREATE OR REPLACE FUNCTION wikore_bump_company_acl_epoch(p_company_id UUID)
RETURNS BIGINT
LANGUAGE sql AS $$
    UPDATE companies SET acl_epoch = acl_epoch + 1
    WHERE  id = p_company_id
    RETURNING acl_epoch;
$$;

-- ===========================================================================
-- 10. §2.7 backfill: enqueue a resync for every active document version so the
--     kSchemaVersion 2->3 payload (acl_epoch / acl_version) is written for the
--     pre-existing corpus. Run ONCE, manually, after this migration loads
--     (NOT auto-run inside the schema-load transaction, so CI smoke tests and
--     large tenants are not forced through a giant enqueue). The outbox worker
--     drains the events at its existing rate; until then those chunks read in
--     G1-fallback mode (correct, just one gate query slower - see §2.7).
--     Returns the number of events enqueued.
-- ===========================================================================

CREATE OR REPLACE FUNCTION wikore_backfill_acl_resync()
RETURNS BIGINT
LANGUAGE plpgsql AS $$
DECLARE
    n BIGINT;
BEGIN
    WITH active AS (
        SELECT dv.company_id,
               dv.document_id,
               dv.id        AS version_id,
               d.acl_version
        FROM   document_versions dv
        JOIN   documents d
            ON d.id = dv.document_id AND d.company_id = dv.company_id
        WHERE  dv.lifecycle_status = 'active'
    ), enq AS (
        INSERT INTO outbox_events
            (company_id, aggregate_id, job_type, job_schema_version,
             payload, idempotency_key)
        SELECT a.company_id, a.document_id, 'qdrant_resync_chunk_acl', 1,
               jsonb_build_object(
                   'document_id',         a.document_id,
                   'document_version_id', a.version_id,
                   'acl_version',         a.acl_version,
                   'reason',              'schema_v3_backfill'),
               'resync:' || a.document_id::text || ':' || a.acl_version::text
        FROM   active a
        ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING
        RETURNING 1
    )
    SELECT count(*) INTO n FROM enq;
    RETURN n;
END;
$$;
