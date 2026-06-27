-- V025: Wiki page tombstones (K2 symmetry with documents)
--
-- Closes the K2 asymmetry: V014 added documents.deleted_at and
-- document_chunk_tombstones for source documents, but wiki_page_versions
-- (which are first-class RAG evidence per the F2 design) had no
-- equivalent. GDPR Article 17 erasure on a wiki page that was synthesized
-- from confidential sources is the same problem class as for source docs.
--
-- Two additions, mirroring V014:
--
--   wiki_pages.deleted_at TIMESTAMPTZ
--     Soft-delete marker. The Qdrant sync job watches this column to
--     trigger vector deletion for all versions of this page. Hard DELETE
--     of the wiki_page row is deferred until after Qdrant confirms
--     deletion (so the sync job can still enumerate affected version IDs).
--
--   wiki_page_version_tombstones
--     Append-only content-free audit. Records that a specific wiki page
--     version's synthesized content was deliberately removed. Stores
--     content_hash only - never the markdown - as proof that the specific
--     content was present and is now gone.
--
-- Granularity note: wiki_page_versions store content as a single TEXT
-- column (not chunked, unlike documents). So the tombstone is per-version,
-- not per-chunk. content_hash matches wiki_page_versions.content_hash.
--
-- No FK on wiki_page_version_id: the row may be hard-deleted by the time
-- the tombstone is written. Same design as V014's document_chunk_tombstones
-- and V007's audit_log - durable records must survive their referents.

-- ---------------------------------------------------------------------------
-- wiki_pages.deleted_at
-- ---------------------------------------------------------------------------

ALTER TABLE wiki_pages
    ADD COLUMN deleted_at TIMESTAMPTZ;

-- Qdrant sync job: find all wiki pages pending deletion.
CREATE INDEX wiki_pages_deleted_idx
    ON wiki_pages (company_id, deleted_at)
    WHERE deleted_at IS NOT NULL;

-- ---------------------------------------------------------------------------
-- wiki_page_version_tombstones
-- ---------------------------------------------------------------------------

CREATE TABLE wiki_page_version_tombstones (
    company_id            UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    wiki_page_version_id  UUID        NOT NULL,
    wiki_page_id          UUID        NOT NULL,           -- recorded for reporting; no FK
    content_hash          TEXT        NOT NULL,           -- SHA-256; proves this content existed
    deleted_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    reason                TEXT,                            -- 'source_deleted', 'gdpr_erasure', etc.

    PRIMARY KEY (company_id, wiki_page_version_id)
);

-- Audit queries: all wiki erasures in a company sorted by time.
CREATE INDEX wiki_page_version_tombstones_company_time_idx
    ON wiki_page_version_tombstones (company_id, deleted_at DESC);

-- Lookup by page: "prove all versions of wiki page P were erased."
CREATE INDEX wiki_page_version_tombstones_page_idx
    ON wiki_page_version_tombstones (company_id, wiki_page_id);

-- ---------------------------------------------------------------------------
-- Append-only enforcement (mirrors V014's document_chunk_tombstones and
-- V007's audit_log). A tombstone is a durable record that specific content
-- was deliberately removed; allowing UPDATE or DELETE would let an attacker
-- (or buggy code) erase the proof of removal, defeating the whole point.
--
-- Defence-in-depth: also revoke at the role level on deploy:
--   REVOKE UPDATE, DELETE ON wiki_page_version_tombstones FROM wikore_app;
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION prevent_wiki_tombstone_mutation()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'wiki_page_version_tombstones is append-only';
END;
$$;

CREATE TRIGGER wiki_page_version_tombstones_no_update
    BEFORE UPDATE ON wiki_page_version_tombstones
    FOR EACH ROW EXECUTE FUNCTION prevent_wiki_tombstone_mutation();

CREATE TRIGGER wiki_page_version_tombstones_no_delete
    BEFORE DELETE ON wiki_page_version_tombstones
    FOR EACH ROW EXECUTE FUNCTION prevent_wiki_tombstone_mutation();
