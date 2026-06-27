-- V003: Documents and chunks
--
-- Documents are the raw evidence ingested into the RAG pipeline.
-- Chunks are the Qdrant-indexed pieces; this table is their registry in Postgres.
--
-- Qdrant collection strategy:
--   ONE collection per embedding model (e.g. "mohio_docs_bge_m3").
--   Access control is enforced via payload filters, not collection separation.
--
-- Qdrant chunk payload:
--   {
--     "company_id":        "<uuid>",
--     "doc_id":            "<uuid>",
--     "chunk_id":          "<uuid>",
--     "owner_org_unit_id": "<uuid>",   -- who manages this document
--     "access_scope_ids":  ["<uuid>"], -- who may retrieve this chunk
--     "lifecycle_status":  "active",
--     "authority_level":   80,
--     "updated_at":        "..."
--   }
--
-- Retrieval filter (applied before any chunk reaches the LLM):
--   company_id        = user.company_id               (exact match)
--   access_scope_ids  intersects user.resolved_scopes  (Match Any)
--   lifecycle_status  = "active"                       (exact match)
--
-- owner_org_unit_id vs access_scope_ids:
--   Ownership (who manages) and visibility (who can retrieve) are separate.
--   HR owns a policy doc; HR + Payroll + Legal can retrieve it.
--   access_scope_ids is recomputed by the application whenever memberships or
--   resource_grants change and synced to Qdrant (see Redis resync queue in V008).
--
-- Deletion semantics on owner_org_unit_id:
--   Intentionally NO ON DELETE action (defaults to RESTRICT). You cannot
--   delete an org_unit that still owns documents. This forces an explicit
--   reassignment or document deletion before the org_unit can be removed.
--   Note: org_units.parent_id CASCADE will try to delete children; if any
--   child owns documents the whole DELETE is rejected, which is correct.

-- ---------------------------------------------------------------------------
-- Documents
-- ---------------------------------------------------------------------------

CREATE TABLE documents (
    id                UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id        UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    owner_org_unit_id UUID        NOT NULL,       -- RESTRICT on delete (see above)
    filename          TEXT        NOT NULL,
    title             TEXT,
    mime_type         TEXT,
    size_bytes        BIGINT,
    source_url        TEXT,
    tags              TEXT[]      NOT NULL DEFAULT '{}',

    -- Pipeline state
    ingest_status     TEXT        NOT NULL DEFAULT 'pending'
                                  CHECK (ingest_status IN (
                                      'pending','processing','done','error'
                                  )),
    chunk_count       INT,                        -- set after successful ingest
    ingested_by       UUID        REFERENCES users(id) ON DELETE SET NULL,
    ingested_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    completed_at      TIMESTAMPTZ,
    error_msg         TEXT,

    -- Evidence lifecycle: only 'active' documents are retrievable.
    lifecycle_status  TEXT        NOT NULL DEFAULT 'draft'
                                  CHECK (lifecycle_status IN (
                                      'draft','active','deprecated','archived'
                                  )),

    -- Reranking weight: 0 = informational, 100 = compliance mandate.
    -- Denormalized into Qdrant payload so the reranker never needs a DB lookup.
    authority_level   INT         NOT NULL DEFAULT 50
                                  CHECK (authority_level BETWEEN 0 AND 100),

    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Required target for composite FKs from document_chunks.
    UNIQUE (company_id, id),

    CONSTRAINT documents_owner_same_company_fk
        FOREIGN KEY (company_id, owner_org_unit_id)
        REFERENCES org_units(company_id, id)
        -- No ON DELETE: RESTRICT is intentional (see header comment).
);

CREATE INDEX documents_company_idx       ON documents (company_id);
CREATE INDEX documents_owner_org_idx     ON documents (owner_org_unit_id);
CREATE INDEX documents_ingest_status_idx ON documents (ingest_status)
    WHERE ingest_status <> 'done';
CREATE INDEX documents_lifecycle_idx     ON documents (lifecycle_status);
CREATE INDEX documents_tags_idx          ON documents USING GIN (tags);

CREATE TRIGGER documents_updated_at
    BEFORE UPDATE ON documents
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Document chunks (Qdrant point registry)
--
-- Postgres is the source of truth for access_scope_ids.
-- When permissions change, this table identifies which Qdrant point IDs need
-- their payload updated without requiring a full reindex.
--
-- access_scope_ids (UUID[]) cannot carry FK constraints (PostgreSQL does not
-- support FKs on array columns). Same-company integrity is enforced by the
-- application when computing and writing access_scope_ids.
-- ---------------------------------------------------------------------------

CREATE TABLE document_chunks (
    id               UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID        NOT NULL,
    document_id      UUID        NOT NULL,
    chunk_index      INT         NOT NULL,
    qdrant_point_id  UUID        NOT NULL UNIQUE,
    content_hash     TEXT        NOT NULL,         -- SHA-256 of chunk text
    access_scope_ids UUID[]      NOT NULL DEFAULT '{}',
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (document_id, chunk_index),
    CONSTRAINT chunks_document_same_company_fk
        FOREIGN KEY (company_id, document_id)
        REFERENCES documents(company_id, id) ON DELETE CASCADE
);

CREATE INDEX document_chunks_document_idx  ON document_chunks (document_id);
CREATE INDEX document_chunks_company_idx   ON document_chunks (company_id);
CREATE INDEX document_chunks_qdrant_idx    ON document_chunks (qdrant_point_id);
-- GIN index for permission-change propagation:
-- find all chunks whose access_scope_ids contain a given org_unit_id.
CREATE INDEX document_chunks_scopes_idx    ON document_chunks USING GIN (access_scope_ids);
