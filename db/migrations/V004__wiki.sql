-- V004: Wiki pages
--
-- Wiki pages are LLM-synthesized structured knowledge surfaces derived from
-- approved documents. They are not freeform user wikis: every page has provenance
-- (source_doc_ids) and inherits access control from its owner org_unit.
--
-- Wiki access follows the same lifecycle model as documents:
--   draft -> active -> deprecated -> archived
-- Only 'active' pages are returned in queries.

CREATE TABLE wiki_pages (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id          UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    org_unit_id         UUID        NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    slug                TEXT        NOT NULL,            -- URL-safe identifier
    title               TEXT        NOT NULL,
    content             TEXT        NOT NULL,            -- markdown
    -- Documents this page was synthesized from (provenance for citations).
    source_doc_ids      UUID[]      NOT NULL DEFAULT '{}',
    lifecycle_status    TEXT        NOT NULL DEFAULT 'draft'
                                    CHECK (lifecycle_status IN (
                                        'draft','active','deprecated','archived'
                                    )),
    created_by          UUID        REFERENCES users(id),
    updated_by          UUID        REFERENCES users(id),
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_unit_id, slug)
);

CREATE INDEX wiki_pages_org_unit_idx  ON wiki_pages (org_unit_id);
CREATE INDEX wiki_pages_company_idx   ON wiki_pages (company_id);
CREATE INDEX wiki_pages_lifecycle_idx ON wiki_pages (lifecycle_status);

CREATE TRIGGER wiki_pages_updated_at
    BEFORE UPDATE ON wiki_pages
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- Link graph: page A references page B within the same org_unit.
-- Used by the wiki lint tool to detect orphaned pages and broken links.
CREATE TABLE wiki_links (
    org_unit_id UUID NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    from_slug   TEXT NOT NULL,
    to_slug     TEXT NOT NULL,
    PRIMARY KEY (org_unit_id, from_slug, to_slug)
);

CREATE INDEX wiki_links_to_idx ON wiki_links (org_unit_id, to_slug);
