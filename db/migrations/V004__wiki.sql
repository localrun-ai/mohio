-- V004: Wiki pages, wiki page versions, and link graph
--
-- Wiki pages are LLM-synthesized knowledge surfaces derived from approved
-- documents. Every page records its source documents (provenance) and
-- inherits access control from its owner org_unit.
--
-- Wiki pages follow the same versioning pattern as documents: each content
-- change creates a new wiki_page_versions row rather than mutating the page.
-- This matters because wiki pages are themselves RAG evidence: a chat turn
-- that cites a wiki page must pin to a specific version_id so the citation
-- remains reproducible even after the page is edited.
--
-- Provenance is stored in wiki_page_sources (join table), not as a UUID[]
-- array column. The join table enforces FK constraints, pins citations to
-- specific document_versions (not mutable document identities), and supports
-- optional chunk-level granularity. Sources now reference wiki_page_version_id
-- rather than wiki_page_id, so citation provenance is locked to the exact
-- content state that was synthesized.

CREATE TABLE wiki_pages (
    id                UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id        UUID        NOT NULL,
    org_unit_id       UUID        NOT NULL,
    slug              TEXT        NOT NULL,
    title             TEXT        NOT NULL,
    lifecycle_status  TEXT        NOT NULL DEFAULT 'draft'
                                  CHECK (lifecycle_status IN (
                                      'draft','active','deprecated','archived'
                                  )),
    created_by        UUID        REFERENCES users(id) ON DELETE SET NULL,
    updated_by        UUID        REFERENCES users(id) ON DELETE SET NULL,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_unit_id, slug),
    -- Required target for composite FKs from wiki_page_versions and wiki_links.
    UNIQUE (company_id, id),
    CONSTRAINT wiki_pages_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

CREATE INDEX wiki_pages_org_unit_idx  ON wiki_pages (org_unit_id);
CREATE INDEX wiki_pages_company_idx   ON wiki_pages (company_id);
CREATE INDEX wiki_pages_lifecycle_idx ON wiki_pages (lifecycle_status);

CREATE TRIGGER wiki_pages_updated_at
    BEFORE UPDATE ON wiki_pages
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE OR REPLACE FUNCTION validate_wiki_pages_actors()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM validate_actor_same_company(NEW.created_by,  NEW.company_id, 'created_by');
    PERFORM validate_actor_same_company(NEW.updated_by,  NEW.company_id, 'updated_by');
    RETURN NEW;
END;
$$;

CREATE TRIGGER wiki_pages_validate_actors
    BEFORE INSERT OR UPDATE ON wiki_pages
    FOR EACH ROW EXECUTE FUNCTION validate_wiki_pages_actors();

-- ---------------------------------------------------------------------------
-- Wiki page versions
--
-- Each content change creates a new wiki_page_versions row. The page identity
-- (wiki_pages.id) is stable; the version captures the content state at a
-- specific point in time.
--
-- Mirrors the document_versions lifecycle model:
--   draft -> active (at most one active version per page, enforced by partial
--   unique index) -> deprecated -> archived.
--
-- wiki_page_sources references wiki_page_version_id so every source citation
-- is pinned to the exact content state that produced it.
-- ---------------------------------------------------------------------------

CREATE TABLE wiki_page_versions (
    id               UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID        NOT NULL,
    wiki_page_id     UUID        NOT NULL,
    version_no       INT         NOT NULL CHECK (version_no > 0),
    content          TEXT        NOT NULL,
    content_hash     TEXT        NOT NULL,
    lifecycle_status TEXT        NOT NULL DEFAULT 'draft'
                                 CHECK (lifecycle_status IN (
                                     'draft','active','deprecated','archived'
                                 )),
    created_by       UUID        REFERENCES users(id) ON DELETE SET NULL,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    activated_at     TIMESTAMPTZ,
    superseded_at    TIMESTAMPTZ,

    UNIQUE (company_id, wiki_page_id, version_no),
    -- Required target for composite FKs from wiki_page_sources.
    UNIQUE (company_id, id),

    CONSTRAINT wiki_page_versions_page_same_company_fk
        FOREIGN KEY (company_id, wiki_page_id)
        REFERENCES wiki_pages(company_id, id) ON DELETE CASCADE
);

-- At most one active version per wiki page per company.
CREATE UNIQUE INDEX wiki_page_versions_one_active_uidx
    ON wiki_page_versions (company_id, wiki_page_id)
    WHERE lifecycle_status = 'active';

CREATE INDEX wiki_page_versions_page_idx      ON wiki_page_versions (company_id, wiki_page_id);
CREATE INDEX wiki_page_versions_lifecycle_idx ON wiki_page_versions (lifecycle_status);

CREATE TRIGGER wiki_page_versions_updated_at
    BEFORE UPDATE ON wiki_page_versions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Wiki page sources (provenance join table)
--
-- Each row records one piece of evidence that contributed to a specific wiki
-- page version. Rows can be at document-version granularity (chunk_id NULL)
-- or chunk granularity (chunk_id NOT NULL).
--
-- Sources reference wiki_page_version_id (not wiki_page_id) so each source
-- citation is pinned to the exact wiki page content state that used it.
--
-- Three-level FK chain enforces referential integrity end-to-end:
--   wps_wiki_page_version_same_company_fk: wiki page version exists in company
--   wps_document_same_company_fk:          document exists in company
--   wps_version_belongs_to_document_fk:    doc version belongs to that document
--   wps_chunk_belongs_to_version_fk:       chunk belongs to that doc version
--
-- All FKs are ON DELETE RESTRICT: cited evidence cannot be deleted while a
-- wiki page version cites it. Archive or re-generate the wiki page first.
--
-- MATCH SIMPLE (PostgreSQL default): when chunk_id IS NULL, the chunk FK
-- is not checked, allowing version-level citations without a specific chunk.
-- ---------------------------------------------------------------------------

CREATE TABLE wiki_page_sources (
    id                   UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id           UUID        NOT NULL,
    wiki_page_version_id UUID        NOT NULL,
    document_id          UUID        NOT NULL,
    document_version_id  UUID        NOT NULL,
    chunk_id             UUID,                        -- NULL = version-level citation

    CONSTRAINT wps_wiki_page_version_same_company_fk
        FOREIGN KEY (company_id, wiki_page_version_id)
        REFERENCES wiki_page_versions(company_id, id) ON DELETE CASCADE,

    CONSTRAINT wps_document_same_company_fk
        FOREIGN KEY (company_id, document_id)
        REFERENCES documents(company_id, id) ON DELETE RESTRICT,

    -- Enforces: doc version belongs to document_id (not just exists in company).
    CONSTRAINT wps_version_belongs_to_document_fk
        FOREIGN KEY (company_id, document_id, document_version_id)
        REFERENCES document_versions(company_id, document_id, id) ON DELETE RESTRICT,

    -- Enforces: chunk belongs to document_version_id (MATCH SIMPLE skips when chunk_id IS NULL).
    CONSTRAINT wps_chunk_belongs_to_version_fk
        FOREIGN KEY (company_id, document_version_id, chunk_id)
        REFERENCES document_chunks(company_id, document_version_id, id) ON DELETE RESTRICT
);

-- Deduplication: one row per (wiki_page_version, doc_version) for version-level
-- citations, one row per (wiki_page_version, doc_version, chunk) for chunk-level.
-- Two partial indexes handle nullable chunk_id without a COALESCE sentinel.
CREATE UNIQUE INDEX wiki_page_sources_version_unique_idx
    ON wiki_page_sources (company_id, wiki_page_version_id, document_version_id)
    WHERE chunk_id IS NULL;

CREATE UNIQUE INDEX wiki_page_sources_chunk_unique_idx
    ON wiki_page_sources (company_id, wiki_page_version_id, document_version_id, chunk_id)
    WHERE chunk_id IS NOT NULL;

CREATE INDEX wiki_page_sources_wiki_ver_idx  ON wiki_page_sources (company_id, wiki_page_version_id);
CREATE INDEX wiki_page_sources_document_idx  ON wiki_page_sources (company_id, document_id);
CREATE INDEX wiki_page_sources_doc_ver_idx   ON wiki_page_sources (company_id, document_version_id);

-- ---------------------------------------------------------------------------
-- Link graph: page A references page B within the same org_unit.
-- Used by the lint tool to detect orphaned pages and broken internal links.
-- Slugs are stored rather than IDs so the graph reflects the actual Markdown
-- link text; the lint tool resolves staleness explicitly.
-- company_id is included to keep repository methods consistent with the
-- rest of the schema (company_id always first in queries and indexes).
CREATE TABLE wiki_links (
    company_id  UUID NOT NULL,
    org_unit_id UUID NOT NULL,
    from_slug   TEXT NOT NULL,
    to_slug     TEXT NOT NULL,

    PRIMARY KEY (company_id, org_unit_id, from_slug, to_slug),

    CONSTRAINT wiki_links_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id)
        ON DELETE CASCADE
);

CREATE INDEX wiki_links_to_idx ON wiki_links (company_id, org_unit_id, to_slug);
