-- V022: Wiki page version lifecycle invariants + promote_wiki_page_version()
--
-- Closes the F2 fix gap. V004 added wiki_page_versions and claimed it
-- "mirrors the document_versions lifecycle model" but the CHECK
-- constraints, lifecycle-timestamp trigger, and atomic promote function
-- (V010's pattern) were never added for wikis. The result: application
-- code can put a wiki_page_versions row into lifecycle_status='active'
-- with NULL activated_at, or attempt activate-first/deprecate-second
-- and hit the non-deferrable partial unique index mid-transaction.
--
-- Iteration 4 ships wiki publishing; iteration 3 wiring chat already
-- depends on wiki_page_sources pointing at versions in a valid state.
-- This migration backs out the asymmetry before that path goes live.
--
-- Three additions, each mechanically analogous to V010 for documents:
--   1. CHECK constraints (active state requires activated_at; deprecated
--      requires superseded_at if previously active; superseded > activated).
--   2. set_wiki_page_version_lifecycle_timestamps() trigger.
--   3. promote_wiki_page_version() SQL function (FOR UPDATE on wiki page
--      row; deprecate-first / activate-second; COALESCE for idempotency;
--      rejects archived).

-- ---------------------------------------------------------------------------
-- 1. Lifecycle CHECK constraints
-- ---------------------------------------------------------------------------

ALTER TABLE wiki_page_versions
    ADD CONSTRAINT wiki_page_versions_active_state_chk CHECK (
        lifecycle_status <> 'active'
        OR (
            activated_at  IS NOT NULL
            AND superseded_at IS NULL
        )
    );

ALTER TABLE wiki_page_versions
    ADD CONSTRAINT wiki_page_versions_superseded_after_activated_chk CHECK (
        activated_at IS NULL
        OR superseded_at IS NULL
        OR superseded_at > activated_at
    );

ALTER TABLE wiki_page_versions
    ADD CONSTRAINT wiki_page_versions_deprecated_interval_chk CHECK (
        lifecycle_status <> 'deprecated'
        OR activated_at IS NULL
        OR superseded_at IS NOT NULL
    );

-- ---------------------------------------------------------------------------
-- 2. Lifecycle timestamp trigger (mirrors V010's set_document_version_...)
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION set_wiki_page_version_lifecycle_timestamps()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'INSERT' THEN
        IF NEW.lifecycle_status = 'active' THEN
            NEW.activated_at  = COALESCE(NEW.activated_at, clock_timestamp());
            NEW.superseded_at = NULL;
        END IF;
        RETURN NEW;
    END IF;

    IF OLD.lifecycle_status IS DISTINCT FROM 'active' AND NEW.lifecycle_status = 'active' THEN
        NEW.activated_at  = COALESCE(NEW.activated_at, clock_timestamp());
        NEW.superseded_at = NULL;
    END IF;

    IF OLD.lifecycle_status = 'active' AND NEW.lifecycle_status IS DISTINCT FROM 'active' THEN
        NEW.superseded_at = COALESCE(NEW.superseded_at, clock_timestamp());
    END IF;

    RETURN NEW;
END;
$$;

CREATE TRIGGER wiki_page_versions_lifecycle_timestamps
    BEFORE INSERT OR UPDATE OF lifecycle_status ON wiki_page_versions
    FOR EACH ROW EXECUTE FUNCTION set_wiki_page_version_lifecycle_timestamps();

-- ---------------------------------------------------------------------------
-- 3. promote_wiki_page_version() - mirrors V010's promote_document_version()
--
-- Atomically promotes a wiki page version to 'active', deprecating the
-- currently active one (if any). FOR UPDATE on the wiki_pages row
-- serializes concurrent promotions. Deprecate-first ordering satisfies
-- the wiki_page_versions_one_active_uidx non-deferrable partial unique
-- index (declared in V004).
--
-- Archived versions are terminal: explicitly reject promotion back from
-- archived so the lifecycle change cannot be silently undone. If
-- rollback is ever needed, introduce an explicit unarchive function so
-- the intent is auditable.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION promote_wiki_page_version(
    p_company_id   UUID,
    p_wiki_page_id UUID,
    p_version_id   UUID
)
RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE
    v_now TIMESTAMPTZ := clock_timestamp();
BEGIN
    -- Lock the wiki page row to serialize concurrent promotions.
    PERFORM 1
    FROM wiki_pages
    WHERE company_id = p_company_id AND id = p_wiki_page_id
    FOR UPDATE;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Wiki page % not found in company %',
            p_wiki_page_id, p_company_id;
    END IF;

    -- Verify the target version exists, belongs to this page, and is not archived.
    PERFORM 1
    FROM wiki_page_versions
    WHERE company_id   = p_company_id
      AND wiki_page_id = p_wiki_page_id
      AND id           = p_version_id
      AND lifecycle_status <> 'archived';

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Wiki version % is not promotable for page % '
            '(not found, wrong page, or archived)',
            p_version_id, p_wiki_page_id;
    END IF;

    -- Step 1: deprecate any currently active version.
    UPDATE wiki_page_versions
    SET lifecycle_status = 'deprecated',
        superseded_at    = COALESCE(superseded_at, v_now)
    WHERE company_id   = p_company_id
      AND wiki_page_id = p_wiki_page_id
      AND lifecycle_status = 'active'
      AND id           <> p_version_id;

    -- Step 2: activate the target version.
    UPDATE wiki_page_versions
    SET lifecycle_status = 'active',
        activated_at     = COALESCE(activated_at, v_now),
        superseded_at    = NULL
    WHERE company_id   = p_company_id
      AND wiki_page_id = p_wiki_page_id
      AND id           = p_version_id;
END;
$$;
