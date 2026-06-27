-- V010: Document version promotion function and lifecycle timestamp trigger
--
-- Two additions that close the version-swap atomicity gap identified in
-- pre-coding review (F1 and F7):
--
-- 1. promote_document_version(): SQL function that atomically deprecates the
--    current active version and promotes a new one. All C++ code that needs to
--    activate a document version MUST call this function; direct UPDATE of
--    lifecycle_status is not permitted in application code.
--
--    Safety properties:
--    - FOR UPDATE lock on the documents row serializes concurrent promotions for
--      the same document (no two promotions can race past each other).
--    - Deprecate-first order: the old active row is set to 'deprecated' before
--      the new row is set to 'active'. This satisfies the partial unique index
--      (document_versions_one_active_per_doc_uidx) even though it is not
--      deferrable; at no point are two active rows present simultaneously.
--    - COALESCE on timestamps: if the application pre-set activated_at or
--      superseded_at, those values are preserved; otherwise clock_timestamp()
--      is used. This keeps the function idempotent for re-runs in case of
--      application retry.
--    - Validates ingest_status = 'done' before promoting to prevent activating
--      a version that has not finished chunking/embedding.
--
-- 2. set_document_version_lifecycle_timestamps(): BEFORE trigger that
--    auto-maintains activated_at and superseded_at as lifecycle_status changes.
--    This is a safety net; promote_document_version() already sets these values
--    explicitly. The trigger ensures they are correct even for any direct UPDATE
--    that bypasses the function (e.g., manual admin corrections).

-- ---------------------------------------------------------------------------
-- promote_document_version(p_company_id, p_document_id, p_version_id)
--
-- Atomically promotes p_version_id to 'active' for the given document,
-- deprecating any currently active version first.
--
-- Raises exceptions for:
--   - Document not found in company
--   - Version not found, not done, or does not belong to the document
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION promote_document_version(
    p_company_id  UUID,
    p_document_id UUID,
    p_version_id  UUID
)
RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE
    v_now TIMESTAMPTZ := clock_timestamp();
BEGIN
    -- Lock the document row to serialize concurrent promotions for the same doc.
    PERFORM 1
    FROM documents
    WHERE company_id = p_company_id AND id = p_document_id
    FOR UPDATE;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Document % not found in company %',
            p_document_id, p_company_id;
    END IF;

    -- Verify the target version exists, belongs to this document, and is ready.
    -- archived versions are terminal: explicitly reject promotion back from
    -- archived so an "archived" lifecycle change cannot be silently undone
    -- by a stray promote call. If rollback to an archived version is ever
    -- needed, introduce an explicit unarchive_document_version() function
    -- so the intent is auditable in code review.
    PERFORM 1
    FROM document_versions
    WHERE company_id    = p_company_id
      AND document_id   = p_document_id
      AND id            = p_version_id
      AND ingest_status = 'done'
      AND lifecycle_status <> 'archived';

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Version % is not promotable for document % '
            '(not found, wrong document, ingest not done, or archived)',
            p_version_id, p_document_id;
    END IF;

    -- Step 1: deprecate any currently active version (must happen before
    -- step 2 to satisfy the non-deferrable partial unique index).
    UPDATE document_versions
    SET lifecycle_status = 'deprecated',
        superseded_at    = COALESCE(superseded_at, v_now)
    WHERE company_id    = p_company_id
      AND document_id   = p_document_id
      AND lifecycle_status = 'active'
      AND id            <> p_version_id;

    -- Step 2: activate the target version.
    UPDATE document_versions
    SET lifecycle_status = 'active',
        activated_at     = COALESCE(activated_at, v_now),
        superseded_at    = NULL
    WHERE company_id    = p_company_id
      AND document_id   = p_document_id
      AND id            = p_version_id;
END;
$$;

-- ---------------------------------------------------------------------------
-- set_document_version_lifecycle_timestamps()
--
-- BEFORE trigger: auto-maintains activated_at and superseded_at whenever
-- lifecycle_status changes. Acts as a safety net for direct UPDATEs; the
-- promote_document_version() function sets these values explicitly, so the
-- trigger is a no-op in the normal path (COALESCE preserves existing values).
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION set_document_version_lifecycle_timestamps()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'INSERT' THEN
        IF NEW.lifecycle_status = 'active' THEN
            NEW.activated_at  = COALESCE(NEW.activated_at, clock_timestamp());
            NEW.superseded_at = NULL;
        END IF;
        RETURN NEW;
    END IF;

    -- UPDATE path: only act when lifecycle_status actually changed.
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

CREATE TRIGGER document_versions_lifecycle_timestamps
    BEFORE INSERT OR UPDATE OF lifecycle_status ON document_versions
    FOR EACH ROW EXECUTE FUNCTION set_document_version_lifecycle_timestamps();
