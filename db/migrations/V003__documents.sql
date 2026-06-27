-- V003: Documents and chunks
--
-- Documents are the raw evidence ingested into the RAG pipeline.
-- Chunks are the vector-indexed pieces stored in Qdrant.
--
-- Qdrant collection strategy: ONE collection per embedding model
-- (e.g. "mohio_docs_bge_m3"), NOT one per org unit. Per Qdrant's own
-- multitenancy guidance, hundreds of collections degrade performance.
-- Access control is enforced via payload filters at query time.
--
-- Each chunk's Qdrant payload carries:
--   {
--     company_id:        "uuid",
--     doc_id:            "uuid",
--     chunk_id:          "uuid",
--     owner_org_unit_id: "uuid",    -- who manages this document
--     access_scope_ids:  ["uuid"],  -- who can retrieve this chunk
--     lifecycle_status:  "active",
--     authority_level:   80,
--     updated_at:        "2026-06-27T00:00:00Z"
--   }
--
-- Retrieval filter applied before any chunk reaches the LLM:
--   must: company_id == user.company_id
--     AND access_scope_ids intersects user.resolved_scope_ids   (Match Any)
--     AND lifecycle_status == "active"
--
-- owner_org_unit_id vs access_scope_ids:
--   ownership and visibility are separate concerns. A document owned by HR
--   can be visible to HR + Payroll + Legal without HR owning those units.
--   access_scope_ids is recomputed whenever memberships or resource_grants
--   change and must be propagated to Qdrant payload (no lazy evaluation).

-- ---------------------------------------------------------------------------
-- Documents
-- ---------------------------------------------------------------------------

CREATE TABLE documents (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    owner_org_unit_id   UUID        NOT NULL REFERENCES org_units(id),
    filename            TEXT        NOT NULL,
    title               TEXT,
    mime_type           TEXT,
    size_bytes          BIGINT,
    source_url          TEXT,                        -- original URL if fetched from web
    tags                TEXT[]      NOT NULL DEFAULT '{}',

    -- Ingestion pipeline state
    ingest_status       TEXT        NOT NULL DEFAULT 'pending'
                                    CHECK (ingest_status IN (
                                        'pending','processing','done','error'
                                    )),
    chunk_count         INT,                         -- set after successful ingest
    ingested_by         UUID        REFERENCES users(id),
    ingested_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    completed_at        TIMESTAMPTZ,
    error_msg           TEXT,

    -- Evidence lifecycle (controls Qdrant filter: only 'active' docs are retrieved)
    lifecycle_status    TEXT        NOT NULL DEFAULT 'draft'
                                    CHECK (lifecycle_status IN (
                                        'draft','active','deprecated','archived'
                                    )),

    -- Authority weight used in reranking (0 = informational, 100 = compliance mandate).
    -- Stored on the chunk payload so the reranker can apply it without a DB lookup.
    authority_level     INT         NOT NULL DEFAULT 50
                                    CHECK (authority_level BETWEEN 0 AND 100),

    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX documents_company_idx       ON documents (company_id);
CREATE INDEX documents_owner_org_idx     ON documents (owner_org_unit_id);
CREATE INDEX documents_ingest_status_idx ON documents (ingest_status) WHERE ingest_status <> 'done';
CREATE INDEX documents_lifecycle_idx     ON documents (lifecycle_status);
CREATE INDEX documents_tags_idx          ON documents USING GIN (tags);

CREATE TRIGGER documents_updated_at
    BEFORE UPDATE ON documents
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Document chunks (Qdrant point registry)
--
-- Postgres is the source of truth for access_scope_ids.
-- When permissions change, this table is used to identify which Qdrant
-- points need their payload updated (without a full reindex).
-- ---------------------------------------------------------------------------

CREATE TABLE document_chunks (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    document_id         UUID        NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
    chunk_index         INT         NOT NULL,
    qdrant_point_id     UUID        NOT NULL UNIQUE,  -- point ID in Qdrant
    content_hash        TEXT        NOT NULL,          -- SHA-256 of chunk text
    -- Snapshot of resolved access scopes at ingest time.
    -- Must be resynced to Qdrant whenever permissions change.
    access_scope_ids    UUID[]      NOT NULL DEFAULT '{}',
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (document_id, chunk_index)
);

CREATE INDEX document_chunks_document_idx   ON document_chunks (document_id);
CREATE INDEX document_chunks_company_idx    ON document_chunks (company_id);
CREATE INDEX document_chunks_qdrant_pt_idx  ON document_chunks (qdrant_point_id);
-- Used for permission-change propagation: find all chunks whose access_scope_ids
-- contain a given org_unit_id so we can push updated payloads to Qdrant.
CREATE INDEX document_chunks_scopes_idx     ON document_chunks USING GIN (access_scope_ids);
