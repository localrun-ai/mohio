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
-- Each row records one document_version that contributed evidence to a wiki
-- page. document_version_id is NOT NULL: citations must pin a specific version
-- so the provenance remains reproducible after the document is superseded.
--
-- document_id is denormalized from document_versions for query convenience
-- ("which wiki pages cite document X?") without requiring a join through
-- document_versions. Must always match document_versions.document_id.
--
-- chunk_id is optional: when present it records the specific chunk(s) cited,
-- giving sub-version granularity for regeneration. It is NOT in the PK so
-- multiple chunks from the same version can each have their own row.
--
-- ON DELETE RESTRICT on both document and version FKs: you cannot delete a
-- document or version that is still cited as evidence by a wiki page. The
-- wiki page must be archived or its sources updated first.
-- ---------------------------------------------------------------------------

CREATE TABLE wiki_page_sources (
    company_id          UUID        NOT NULL,
    wiki_page_id        UUID        NOT NULL,
    document_id         UUID        NOT NULL,
    document_version_id UUID        NOT NULL,
    chunk_id            UUID,                         -- optional chunk granularity

    PRIMARY KEY (wiki_page_id, document_version_id),

    CONSTRAINT wps_wiki_page_same_company_fk
        FOREIGN KEY (company_id, wiki_page_id)
        REFERENCES wiki_pages(company_id, id) ON DELETE CASCADE,

    CONSTRAINT wps_document_same_company_fk
        FOREIGN KEY (company_id, document_id)
        REFERENCES documents(company_id, id) ON DELETE RESTRICT,

    CONSTRAINT wps_version_same_company_fk
        FOREIGN KEY (company_id, document_version_id)
        REFERENCES document_versions(company_id, id) ON DELETE RESTRICT,

    -- chunk_id is nullable; FK is checked only when non-NULL.
    CONSTRAINT wps_chunk_same_company_fk
        FOREIGN KEY (company_id, chunk_id)
        REFERENCES document_chunks(company_id, id) ON DELETE SET NULL
);

CREATE INDEX wiki_page_sources_page_idx     ON wiki_page_sources (wiki_page_id);
CREATE INDEX wiki_page_sources_document_idx ON wiki_page_sources (company_id, document_id);
CREATE INDEX wiki_page_sources_version_idx  ON wiki_page_sources (document_version_id);

-- ---------------------------------------------------------------------------
-- Link graph: page A references page B within the same org_unit.
-- Used by the lint tool to detect orphaned pages and broken internal links.
-- Slugs are stored rather than IDs so the graph reflects the actual Markdown
-- link text; the lint tool resolves staleness explicitly.
CREATE TABLE wiki_links (
    org_unit_id UUID NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    from_slug   TEXT NOT NULL,
    to_slug     TEXT NOT NULL,
    PRIMARY KEY (org_unit_id, from_slug, to_slug)
);

CREATE INDEX wiki_links_to_idx ON wiki_links (org_unit_id, to_slug);
