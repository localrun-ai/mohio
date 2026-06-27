-- V004: Wiki pages and link graph
--
-- Wiki pages are LLM-synthesized knowledge surfaces derived from approved
-- documents. Every page records its source documents (provenance) and
-- inherits access control from its owner org_unit.
--
-- source_doc_ids (UUID[]) cannot carry FK constraints; same-company integrity
-- and existence are enforced by the application when generating pages.

CREATE TABLE wiki_pages (
    id               UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID        NOT NULL,
    org_unit_id      UUID        NOT NULL,
    slug             TEXT        NOT NULL,
    title            TEXT        NOT NULL,
    content          TEXT        NOT NULL,        -- markdown
    source_doc_ids   UUID[]      NOT NULL DEFAULT '{}',  -- provenance; no FK (array)
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
