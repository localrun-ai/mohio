-- V003: Documents and document versions
--
-- Documents are the raw evidence ingested into the RAG pipeline.
-- Every content change creates a new document_version row rather than
-- mutating the existing record. This preserves citation reproducibility:
-- chat turns and wiki pages pin to a specific version_id, so historical
-- answers remain verifiable even after an SOP is superseded.
--
-- Lifecycle model:
--   document_versions.lifecycle_status drives retrieval, not documents.
--   All versions with ingest_status='done' are indexed in Qdrant; the
--   lifecycle_status field is stored in the payload and used as a filter.
--   A partial unique index enforces at most one active version per document.
--   When a new version is activated, the previous version becomes 'deprecated'
--   (its payload lifecycle_status is updated in Qdrant; chunks remain indexed).
--   'archived' = excluded from all retrieval including historical queries.
--
-- Qdrant collection strategy:
--   ONE collection per embedding model (e.g. "wikore_docs_bge_m3").
--   Access control is enforced via payload filters, not collection separation.
--
-- Qdrant chunk payload (stored in each collection under embedding_models):
--   {
--     "company_id":          "<uuid>",
--     "doc_id":              "<uuid>",
--     "document_version_id": "<uuid>",
--     "chunk_id":            "<uuid>",
--     "owner_org_unit_id":   "<uuid>",          -- who manages this document
--     "access_scope_ids":    ["<uuid>"],         -- which org_units may retrieve this chunk
--     "lifecycle_status":    "active",           -- from document_versions
--     "authority_level":     80,                 -- from documents; 0=informational, 100=mandate
--     "activated_at":        "2026-06-01T00:00:00Z",  -- when this version became active; null if never activated
--     "superseded_at":       null,               -- when superseded by a newer version; null if still current
--     "updated_at":          "..."               -- last payload sync time
--   }
--
-- activated_at and superseded_at are required in the payload so C++ can apply
-- historical as-of filters entirely within Qdrant without a Postgres round-trip.
--
-- Each embedding model has its own Qdrant collection (embedding_models.qdrant_collection).
-- document_chunk_vectors maps chunk_id -> qdrant_point_id per model.
--
-- Retrieval filters:
--   Default (current knowledge):
--     company_id        = user.company_id               (exact match)
--     access_scope_ids  intersects user.resolved_scopes  (Match Any)
--     lifecycle_status  = "active"                       (exact match)
--
--   Historical / as-of (e.g. "what was policy on 2026-05-15?"):
--     company_id        = user.company_id               (exact match)
--     access_scope_ids  intersects user.resolved_scopes  (Match Any)
--     lifecycle_status  in ["active", "deprecated"]      (Match Any)
--     activated_at      <= target_date                   (range)
--     superseded_at     > target_date OR is null         (range)
--
--   'archived' versions are never returned by either filter.
--
-- owner_org_unit_id vs access_scope_ids:
--   Ownership (who manages) and visibility (who can retrieve) are separate.
--   HR owns a policy doc; HR + Payroll + Legal can retrieve it.
--   access_scope_ids is recomputed when document visibility changes:
--   resource_grant create/revoke, document owner_org_unit change, org-unit
--   moves that affect inherited grants, or lifecycle changes. User/group
--   membership changes do NOT rewrite access_scope_ids; they only invalidate
--   lr:eff cache keys (see Redis invalidation model in V008).
--
-- Deletion semantics on owner_org_unit_id:
--   Intentionally NO ON DELETE action (defaults to RESTRICT). You cannot
--   delete an org_unit that still owns documents. This forces an explicit
--   reassignment or document deletion before the org_unit can be removed.

-- ---------------------------------------------------------------------------
-- Documents (identity record; version-independent metadata)
-- ---------------------------------------------------------------------------

CREATE TABLE documents (
    id                UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id        UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    owner_org_unit_id UUID        NOT NULL,       -- RESTRICT on delete (see above)
    filename          TEXT        NOT NULL,
    title             TEXT,
    mime_type         TEXT,
    tags              TEXT[]      NOT NULL DEFAULT '{}',

    -- Reranking weight: 0 = informational, 100 = compliance mandate.
    -- Denormalized into Qdrant payload so the reranker never needs a DB lookup.
    authority_level   INT         NOT NULL DEFAULT 50
                                  CHECK (authority_level BETWEEN 0 AND 100),

    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Required target for composite FKs from document_versions and chunks.
    UNIQUE (company_id, id),

    CONSTRAINT documents_owner_same_company_fk
        FOREIGN KEY (company_id, owner_org_unit_id)
        REFERENCES org_units(company_id, id)
        -- No ON DELETE: RESTRICT is intentional (see header comment).
);

CREATE INDEX documents_company_idx       ON documents (company_id);
CREATE INDEX documents_owner_org_idx     ON documents (owner_org_unit_id);
CREATE INDEX documents_tags_idx          ON documents USING GIN (tags);

CREATE TRIGGER documents_updated_at
    BEFORE UPDATE ON documents
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Document versions
--
-- Each upload or content change produces a new version_no. The document
-- identity (documents.id) is stable; the version captures the content state.
--
-- created_by: simple FK (not composite) to avoid ON DELETE SET NULL nulling
-- out company_id (same pattern as memberships.granted_by).
--
-- At most one version per document may have lifecycle_status = 'active'.
-- Promoting a new version must also set the previous active version to
-- 'deprecated' in the same transaction.
-- ---------------------------------------------------------------------------

CREATE TABLE document_versions (
    id               UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID        NOT NULL,
    document_id      UUID        NOT NULL,
    version_no       INT         NOT NULL CHECK (version_no > 0),
    source_hash      TEXT        NOT NULL,        -- SHA-256 of the source file
    source_uri       TEXT,                        -- storage path / URL for this file
    size_bytes       BIGINT      CHECK (size_bytes >= 0),
    ingest_status    TEXT        NOT NULL DEFAULT 'pending'
                                 CHECK (ingest_status IN (
                                     'pending','processing','done','error'
                                 )),
    chunk_count      INT         CHECK (chunk_count >= 0),
    created_by       UUID        REFERENCES users(id) ON DELETE SET NULL,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    completed_at     TIMESTAMPTZ,
    activated_at     TIMESTAMPTZ,   -- set when version transitions to 'active'
    superseded_at    TIMESTAMPTZ,   -- set when a newer version becomes 'active'
    error_msg        TEXT,
    lifecycle_status TEXT        NOT NULL DEFAULT 'draft'
                                 CHECK (lifecycle_status IN (
                                     'draft','active','deprecated','archived'
                                 )),

    -- An active version must be fully ingested, have a known activation time,
    -- and must not yet be superseded.
    CONSTRAINT document_versions_active_state_chk CHECK (
        lifecycle_status <> 'active'
        OR (
            ingest_status   = 'done'
            AND completed_at  IS NOT NULL
            AND activated_at  IS NOT NULL
            AND superseded_at IS NULL
        )
    ),
    -- A done ingest must record when it finished and how many chunks it produced.
    CONSTRAINT document_versions_done_state_chk CHECK (
        ingest_status <> 'done'
        OR (completed_at IS NOT NULL AND chunk_count IS NOT NULL)
    ),
    -- Active lifecycle requires completed ingest (implied by active_state_chk above,
    -- stated explicitly here for clarity when reading the constraint list in isolation).
    CONSTRAINT document_versions_active_requires_done_chk CHECK (
        lifecycle_status <> 'active'
        OR ingest_status = 'done'
    ),

    UNIQUE (company_id, document_id, version_no),
    -- (company_id, id): target for composite FKs that only need version identity.
    UNIQUE (company_id, id),
    -- (company_id, document_id, id): target for composite FKs that must also enforce
    -- the version belongs to a specific document (used by wiki_page_sources).
    UNIQUE (company_id, document_id, id),

    CONSTRAINT document_versions_document_same_company_fk
        FOREIGN KEY (company_id, document_id)
        REFERENCES documents(company_id, id) ON DELETE CASCADE
);

CREATE INDEX document_versions_document_idx  ON document_versions (document_id);
CREATE INDEX document_versions_company_idx   ON document_versions (company_id);
CREATE INDEX document_versions_ingest_idx    ON document_versions (ingest_status)
    WHERE ingest_status <> 'done';
CREATE INDEX document_versions_lifecycle_idx ON document_versions (lifecycle_status);
-- Point-in-time lookup: "which version was active on date D for this document?"
--   WHERE company_id=$c AND document_id=$d
--     AND activated_at <= $d AND (superseded_at IS NULL OR superseded_at > $d)
CREATE INDEX document_versions_asof_idx
    ON document_versions (company_id, document_id, activated_at, superseded_at);

-- Enforce: at most one active version per document per company.
CREATE UNIQUE INDEX document_versions_one_active_per_doc_uidx
    ON document_versions (company_id, document_id)
    WHERE lifecycle_status = 'active';

CREATE OR REPLACE FUNCTION validate_document_versions_actors()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM validate_actor_same_company(NEW.created_by, NEW.company_id, 'created_by');
    RETURN NEW;
END;
$$;

CREATE TRIGGER document_versions_validate_actors
    BEFORE INSERT OR UPDATE ON document_versions
    FOR EACH ROW EXECUTE FUNCTION validate_document_versions_actors();

CREATE TRIGGER document_versions_updated_at
    BEFORE UPDATE ON document_versions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Embedding models (global server configuration; not company-scoped)
--
-- Each enabled model maps to exactly one Qdrant collection. The schema
-- supports multiple simultaneous models (e.g. bge-m3 for multilingual,
-- e5-large for English-heavy corpora), so re-indexing with a new model
-- does not require dropping or migrating existing collections.
--
-- ON DELETE RESTRICT on document_chunk_vectors.embedding_model_id ensures
-- you cannot drop a model that still has indexed vectors.
-- ---------------------------------------------------------------------------

CREATE TABLE embedding_models (
    id                UUID    PRIMARY KEY DEFAULT gen_random_uuid(),
    name              TEXT    NOT NULL UNIQUE,      -- e.g. 'bge-m3', 'e5-large'
    qdrant_collection TEXT    NOT NULL UNIQUE,      -- e.g. 'wikore_docs_bge_m3'
    dimension         INT     NOT NULL,
    enabled           BOOLEAN NOT NULL DEFAULT true
);

-- ---------------------------------------------------------------------------
-- Document chunks (evidence unit; model-agnostic)
--
-- Chunks belong to a specific document_version, not the document itself.
-- This means historical chat citations (which store document_version_id)
-- remain resolvable even after the document is superseded by a new version.
--
-- Postgres is the source of truth for access_scope_ids.
-- When permissions change, this table identifies which chunks need their
-- Qdrant payload updated without requiring a full reindex.
--
-- access_scope_ids (UUID[]) cannot carry FK constraints (PostgreSQL does not
-- support FKs on array columns). Same-company integrity is enforced by the
-- application when computing and writing access_scope_ids.
--
-- UNIQUE (company_id, id) exposes a composite FK target for
-- document_chunk_vectors so same-company membership can be enforced.
-- ---------------------------------------------------------------------------

CREATE TABLE document_chunks (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID        NOT NULL,
    document_version_id UUID        NOT NULL,
    chunk_index         INT         NOT NULL,
    content             TEXT        NOT NULL,         -- exact chunk text passed to the LLM
    content_hash        TEXT        NOT NULL,         -- SHA-256 of content; used for dedup and diff
    access_scope_ids    UUID[]      NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (document_version_id, chunk_index),
    -- (company_id, id): target for FKs that only need chunk identity.
    UNIQUE (company_id, id),
    -- (company_id, document_version_id, id): target for FKs that must also enforce
    -- the chunk belongs to a specific version (used by wiki_page_sources).
    UNIQUE (company_id, document_version_id, id),
    CONSTRAINT chunks_version_same_company_fk
        FOREIGN KEY (company_id, document_version_id)
        REFERENCES document_versions(company_id, id) ON DELETE CASCADE
);

CREATE INDEX document_chunks_version_idx   ON document_chunks (document_version_id);
CREATE INDEX document_chunks_company_idx   ON document_chunks (company_id);
-- GIN index for permission-change propagation:
-- find all chunks whose access_scope_ids contain a given org_unit_id.
CREATE INDEX document_chunks_scopes_idx    ON document_chunks USING GIN (access_scope_ids);

-- ---------------------------------------------------------------------------
-- Document chunk vectors (one row per chunk per embedding model)
--
-- Separating vectors from chunks allows multiple embedding models to coexist.
-- Each model indexes the same chunk text into its own Qdrant collection.
--
-- qdrant_point_id is unique per collection (model), not globally, so the
-- uniqueness constraint is (embedding_model_id, qdrant_point_id).
--
-- Cascade chain: companies -> documents -> document_versions ->
-- document_chunks -> document_chunk_vectors (ON DELETE CASCADE at each step).
-- ---------------------------------------------------------------------------

CREATE TABLE document_chunk_vectors (
    company_id         UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    chunk_id           UUID        NOT NULL,
    embedding_model_id UUID        NOT NULL REFERENCES embedding_models(id) ON DELETE RESTRICT,
    qdrant_point_id    UUID        NOT NULL,
    embedded_at        TIMESTAMPTZ NOT NULL DEFAULT now(),

    PRIMARY KEY (chunk_id, embedding_model_id),
    -- Point IDs are unique within a Qdrant collection (= per model).
    UNIQUE (embedding_model_id, qdrant_point_id),
    -- Composite FK enforces chunk belongs to the same company as this vector row.
    CONSTRAINT chunk_vectors_chunk_same_company_fk
        FOREIGN KEY (company_id, chunk_id)
        REFERENCES document_chunks(company_id, id) ON DELETE CASCADE
);

CREATE INDEX document_chunk_vectors_chunk_idx  ON document_chunk_vectors (chunk_id);
CREATE INDEX document_chunk_vectors_model_idx  ON document_chunk_vectors (embedding_model_id);
CREATE INDEX document_chunk_vectors_company_idx ON document_chunk_vectors (company_id);
