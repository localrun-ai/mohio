-- Wiki pages: LLM-synthesized structured knowledge, scoped per org.
-- Content stored in DB (not flat files) so access control is enforced at query time.

CREATE TABLE wiki_pages (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    slug        TEXT        NOT NULL,            -- URL-safe identifier
    title       TEXT        NOT NULL,
    content     TEXT        NOT NULL,            -- markdown
    source_docs UUID[]      NOT NULL DEFAULT '{}', -- document IDs this page was derived from
    created_by  UUID        REFERENCES users(id),
    updated_by  UUID        REFERENCES users(id),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, slug)
);

CREATE INDEX wiki_pages_org_id_idx ON wiki_pages (org_id);

CREATE TRIGGER wiki_pages_updated_at
    BEFORE UPDATE ON wiki_pages
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- Wiki link graph (page A links to page B within same org)
-- Used by lint to detect orphaned pages and broken links.
CREATE TABLE wiki_links (
    org_id      UUID    NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    from_slug   TEXT    NOT NULL,
    to_slug     TEXT    NOT NULL,
    PRIMARY KEY (org_id, from_slug, to_slug)
);

CREATE INDEX wiki_links_to_idx ON wiki_links (org_id, to_slug);
