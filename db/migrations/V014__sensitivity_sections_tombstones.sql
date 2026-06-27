-- V014: Sensitivity labels (K1), section hierarchy (K5), deletion tombstones (K2)
--
-- Three schema primitives that must exist before C++ retrieval coding starts
-- because they affect the Qdrant payload contract and/or the ingest pipeline.
--
-- K1 SENSITIVITY LABELS
--   Sensitivity is orthogonal to org-unit access. An HR policy can be
--   org-scoped to HR (who sees it) AND labelled 'confidential' (how it may
--   be used). Retrofitting sensitivity after customer data exists means
--   classifying the entire corpus, which is painful at enterprise scale.
--
--   Two-level model:
--     documents.sensitivity_label_default: set at document creation; new
--       document_versions inherit it but can override.
--     document_versions.sensitivity_label: the actual label used in the
--       Qdrant payload and retrieval filter. C++ sets this at version-creation
--       time, defaulting to the document's current label_default.
--
--   Same model for wiki_pages / wiki_page_versions.
--
--   Retrieval semantics (C++ enforces; DB stores):
--     'public'       - visible in all sessions, including guest/external
--     'internal'     - visible to all members of the company (default)
--     'confidential' - visible only to explicitly scoped org_units/grants
--     'restricted'   - never included in generated answers; only direct browse
--
-- K5 SECTION HIERARCHY
--   Chunking without section context produces disconnected evidence: "chunk
--   247 of policy.pdf". With a section tree, the retriever can cite "§3.2.1",
--   expand to parent or sibling chunks for high-importance hits, and surface
--   navigable in-context references. Retrofitting requires re-ingesting the
--   entire corpus. Adding the schema now costs nothing if the section parser
--   is not yet built; document_chunks.section_id is nullable so plain chunkers
--   still produce valid rows.
--
--   Parent FK uses a composite (company_id, document_version_id, parent_section_id)
--   reference so PostgreSQL enforces that a section's parent is in the same
--   document version. Version consistency between sections and their referencing
--   chunks is ingest-pipeline enforced (section_id is a simple FK to
--   document_sections(id); the ingest pipeline assigns it only to same-version
--   sections).
--
-- K2 TOMBSTONES (minimal)
--   GDPR Article 17 (right to erasure) and audit integrity pull in opposite
--   directions. documents.deleted_at marks intent; the Qdrant sync job uses it
--   to trigger vector deletion. document_chunk_tombstones keeps a content-free
--   audit record (hash only, never text) proving that specific chunk content
--   was deliberately removed. The full erasure workflow and nightly sweep job
--   are out of scope here and will be built as a separate operational service.

-- ===========================================================================
-- K1: Sensitivity labels
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- documents: document-level default label
-- ---------------------------------------------------------------------------
ALTER TABLE documents
    ADD COLUMN sensitivity_label_default TEXT NOT NULL DEFAULT 'internal'
        CHECK (sensitivity_label_default IN ('public','internal','confidential','restricted'));

-- ---------------------------------------------------------------------------
-- document_versions: version-level label (included in Qdrant payload)
-- ---------------------------------------------------------------------------
ALTER TABLE document_versions
    ADD COLUMN sensitivity_label TEXT NOT NULL DEFAULT 'internal'
        CHECK (sensitivity_label IN ('public','internal','confidential','restricted'));

-- Retrieval filter support: "show all restricted documents" admin query;
-- also the hot path when the C++ retrieval filter includes sensitivity_label.
CREATE INDEX document_versions_sensitivity_idx
    ON document_versions (company_id, sensitivity_label);

-- ---------------------------------------------------------------------------
-- wiki_pages and wiki_page_versions: same two-level pattern
-- ---------------------------------------------------------------------------
ALTER TABLE wiki_pages
    ADD COLUMN sensitivity_label_default TEXT NOT NULL DEFAULT 'internal'
        CHECK (sensitivity_label_default IN ('public','internal','confidential','restricted'));

ALTER TABLE wiki_page_versions
    ADD COLUMN sensitivity_label TEXT NOT NULL DEFAULT 'internal'
        CHECK (sensitivity_label IN ('public','internal','confidential','restricted'));

-- ===========================================================================
-- K5: Document section hierarchy
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- document_sections
--
-- Stores the heading/section tree extracted during ingest. Each section
-- belongs to a specific document_version. The ordinal is document-global
-- (sequential through the entire document, not per-parent), so the full
-- document order is recoverable without recursion.
--
-- heading_path is the breadcrumb array: ['Chapter 3', '§3.2', '§3.2.1'].
-- heading is the section's own title (last element of heading_path, or NULL
-- for content that precedes the first heading in the document).
--
-- Cascade contract:
--   Version deleted -> document_sections cascade.
--   Parent section deleted -> child sections cascade (correct: the tree is
--     always deleted top-down as part of version deletion).
--
-- UNIQUE (company_id, document_version_id, id) exposes a composite FK target
-- for C++ queries that need to enforce same-version membership. The parent FK
-- uses this target to enforce parent-is-same-version at the DB level.
-- ---------------------------------------------------------------------------

CREATE TABLE document_sections (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID        NOT NULL,
    document_version_id UUID        NOT NULL,
    parent_section_id   UUID,                        -- NULL for top-level sections
    ordinal             INT         NOT NULL,         -- sequential position in document
    heading             TEXT,                         -- this section's title (NULL before first heading)
    heading_path        TEXT[]      NOT NULL DEFAULT '{}',  -- full breadcrumb, e.g. ['Chapter 3','§3.2.1']
    depth               INT         NOT NULL CHECK (depth >= 0),

    -- (company_id, id): target for simple same-company FKs.
    UNIQUE (company_id, id),
    -- (company_id, document_version_id, id): target for version-consistent FKs.
    UNIQUE (company_id, document_version_id, id),
    -- ordinal is unique within a version (global sequential numbering).
    UNIQUE (company_id, document_version_id, ordinal),

    -- Version must belong to the company.
    CONSTRAINT document_sections_version_same_company_fk
        FOREIGN KEY (company_id, document_version_id)
        REFERENCES document_versions(company_id, id) ON DELETE CASCADE,

    -- Parent must be in the same version (enforces tree coherence).
    -- MATCH SIMPLE: when parent_section_id IS NULL (top-level), FK is not checked.
    CONSTRAINT document_sections_parent_same_version_fk
        FOREIGN KEY (company_id, document_version_id, parent_section_id)
        REFERENCES document_sections(company_id, document_version_id, id)
        ON DELETE CASCADE
);

-- All sections in a version (tree walk, chunk joins).
CREATE INDEX document_sections_version_idx
    ON document_sections (company_id, document_version_id, ordinal);

-- Parent -> children traversal.
CREATE INDEX document_sections_parent_idx
    ON document_sections (company_id, parent_section_id)
    WHERE parent_section_id IS NOT NULL;

-- ---------------------------------------------------------------------------
-- document_chunks: add nullable section_id
--
-- section_id points to the document_section this chunk belongs to.
-- NULL is valid: plain chunkers that don't parse section structure still
-- produce correct rows.
--
-- The FK is a simple (section_id -> document_sections.id) rather than a
-- composite FK. The composite UNIQUE (company_id, document_version_id, id)
-- on document_sections is exposed for C++ query joins. Version consistency
-- (chunk and section belong to the same document_version) is enforced by
-- the ingest pipeline, which is the only writer of chunk-section links.
--
-- ON DELETE SET NULL: if a specific section is deleted without deleting the
-- whole version, referencing chunk rows keep their content but lose the section
-- pointer. In practice, sections are always deleted via version cascade, so
-- chunks cascade first (from version) and this ON DELETE never fires.
-- ---------------------------------------------------------------------------

ALTER TABLE document_chunks
    ADD COLUMN section_id UUID
        REFERENCES document_sections(id) ON DELETE SET NULL;

CREATE INDEX document_chunks_section_idx
    ON document_chunks (company_id, section_id)
    WHERE section_id IS NOT NULL;

-- ===========================================================================
-- K2: Deletion tombstones (minimal primitive; full erasure workflow deferred)
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- documents.deleted_at
--
-- Set when a document is marked for deletion or erasure.
-- The Qdrant sync job watches this column to trigger vector deletion for
-- all chunk_ids belonging to versions of this document.
-- Hard DELETE of the document row is deferred until after Qdrant confirms
-- deletion (so the sync job can still enumerate affected chunk_ids).
-- ---------------------------------------------------------------------------

ALTER TABLE documents
    ADD COLUMN deleted_at TIMESTAMPTZ;

-- Qdrant sync job: find all documents pending deletion.
CREATE INDEX documents_deleted_idx
    ON documents (company_id, deleted_at)
    WHERE deleted_at IS NOT NULL;

-- ---------------------------------------------------------------------------
-- document_chunk_tombstones
--
-- Append-only audit record that a specific chunk's content was deliberately
-- removed from Qdrant. Never stores chunk text — only content_hash as proof
-- that the specific content was present and is now gone.
--
-- company_id and document_version_id are recorded for reporting
-- ("show all erasures for document X" admin query) without needing to
-- join back to a potentially-deleted document row.
--
-- No FK on chunk_id: the chunk row may already be hard-deleted by the time
-- the tombstone is written. This is intentional (same design decision as the
-- audit_log).
-- ---------------------------------------------------------------------------

CREATE TABLE document_chunk_tombstones (
    company_id          UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    chunk_id            UUID        NOT NULL,
    document_version_id UUID        NOT NULL,
    content_hash        TEXT        NOT NULL,    -- SHA-256; proves this content existed
    deleted_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    reason              TEXT,                    -- 'source_deleted', 'gdpr_erasure', etc.

    PRIMARY KEY (company_id, chunk_id)
);

-- Audit queries: all erasures in a company sorted by time.
CREATE INDEX document_chunk_tombstones_company_time_idx
    ON document_chunk_tombstones (company_id, deleted_at DESC);

-- Lookup by version: "prove all chunks from version V were erased."
CREATE INDEX document_chunk_tombstones_version_idx
    ON document_chunk_tombstones (company_id, document_version_id);
