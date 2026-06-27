-- V004: Wiki pages and link graph
--
-- Wiki pages are LLM-synthesized knowledge surfaces derived from approved
-- documents. Every page records its source documents (provenance) and
-- inherits access control from its owner org_unit.
--
-- Provenance is stored in wiki_page_sources (join table), not as a UUID[]
-- array column. The join table enforces FK constraints, pins citations to
-- specific document_versions (not mutable document identities), and supports
-- optional chunk-level granularity.

CREATE TABLE wiki_pages (
    id                UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id        UUID        NOT NULL,
    org_unit_id       UUID        NOT NULL,
    slug              TEXT        NOT NULL,
    title             TEXT        NOT NULL,
    content           TEXT        NOT NULL,        -- markdown
    lifecycle_status TEXT        NOT NULL DEFAULT 'draft'
                                 CHECK (lifecycle_status IN (
                                     'draft','active','deprecated','archived'
                                 )),
    created_by       UUID        REFERENCES users(id) ON DELETE SET NULL,
    updated_by       UUID        REFERENCES users(id) ON DELETE SET NULL,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_unit_id, slug),
    -- Required target for composite FKs from wiki_links.
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
-- Wiki page sources (provenance join table)
--
-- Each row records one piece of evidence that contributed to a wiki page.
-- Rows can be at version granularity (chunk_id NULL) or chunk granularity
-- (chunk_id NOT NULL). Multiple chunks from the same version are allowed,
-- each as a separate row - enforced by a functional unique index rather than
-- the PK (surrogate UUID) so nullable chunk_id is handled correctly.
--
-- Three-level FK chain enforces referential integrity end-to-end:
--   wps_document_same_company_fk:     document exists in company
--   wps_version_belongs_to_document_fk: version belongs to that document
--   wps_chunk_belongs_to_version_fk:  chunk belongs to that version
--
-- All FKs are ON DELETE RESTRICT: cited evidence cannot be deleted while a
-- wiki page cites it. Archive or re-generate the wiki page first.
--
-- MATCH SIMPLE (PostgreSQL default): when chunk_id IS NULL, the chunk FK
-- is not checked, allowing version-level citations without a specific chunk.
-- ---------------------------------------------------------------------------

CREATE TABLE wiki_page_sources (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID        NOT NULL,
    wiki_page_id        UUID        NOT NULL,
    document_id         UUID        NOT NULL,
    document_version_id UUID        NOT NULL,
    chunk_id            UUID,                         -- NULL = version-level citation

    CONSTRAINT wps_wiki_page_same_company_fk
        FOREIGN KEY (company_id, wiki_page_id)
        REFERENCES wiki_pages(company_id, id) ON DELETE CASCADE,

    CONSTRAINT wps_document_same_company_fk
        FOREIGN KEY (company_id, document_id)
        REFERENCES documents(company_id, id) ON DELETE RESTRICT,

    -- Enforces: version belongs to document_id (not just exists in company).
    CONSTRAINT wps_version_belongs_to_document_fk
        FOREIGN KEY (company_id, document_id, document_version_id)
        REFERENCES document_versions(company_id, document_id, id) ON DELETE RESTRICT,

    -- Enforces: chunk belongs to document_version_id (MATCH SIMPLE skips when chunk_id IS NULL).
    CONSTRAINT wps_chunk_belongs_to_version_fk
        FOREIGN KEY (company_id, document_version_id, chunk_id)
        REFERENCES document_chunks(company_id, document_version_id, id) ON DELETE RESTRICT
);

-- Deduplication: one row per (wiki_page, version, chunk) combination.
-- COALESCE maps NULL chunk_id to a sentinel so the index treats version-level
-- citations as a single entry per version rather than allowing unlimited duplicates.
CREATE UNIQUE INDEX wiki_page_sources_unique_source_idx
    ON wiki_page_sources (
        company_id,
        wiki_page_id,
        document_version_id,
        COALESCE(chunk_id, '00000000-0000-0000-0000-000000000000'::uuid)
    );

CREATE INDEX wiki_page_sources_page_idx     ON wiki_page_sources (company_id, wiki_page_id);
CREATE INDEX wiki_page_sources_document_idx ON wiki_page_sources (company_id, document_id);
CREATE INDEX wiki_page_sources_version_idx  ON wiki_page_sources (company_id, document_version_id);

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
