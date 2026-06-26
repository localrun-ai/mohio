-- Documents ingested into the RAG pipeline.
-- Actual content lives in Qdrant (chunks) and on disk (original file).
-- Qdrant collection per org: kb_{org_id}
-- Disk path: /data/orgs/{org_id}/uploads/{doc_id}/{filename}

CREATE TABLE documents (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    filename        TEXT        NOT NULL,
    mime_type       TEXT,
    size_bytes      BIGINT,
    status          TEXT        NOT NULL DEFAULT 'pending'
                                CHECK (status IN ('pending','processing','done','error')),
    chunk_count     INT,                        -- set after successful ingest
    ingested_by     UUID        REFERENCES users(id),
    ingested_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    completed_at    TIMESTAMPTZ,
    error_msg       TEXT,
    -- Metadata for filtering/display
    title           TEXT,
    tags            TEXT[]      NOT NULL DEFAULT '{}',
    source_url      TEXT        -- original URL if fetched from web
);

CREATE INDEX documents_org_id_idx    ON documents (org_id);
CREATE INDEX documents_status_idx    ON documents (status) WHERE status <> 'done';
CREATE INDEX documents_tags_idx      ON documents USING GIN (tags);
