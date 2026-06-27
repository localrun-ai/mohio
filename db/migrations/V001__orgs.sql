-- V001: Companies and org units
--
-- Companies are first-class tenants (hard isolation boundary).
-- Org units form a tree within a company:
--   [subsidiary] -> [division] -> department -> [team | project]
--
-- Two key design decisions for enterprise RAG:
--
-- 1. company_id is denormalized on every org_unit row so Qdrant payload
--    filters can scope by company without a tree traversal at query time.
--
-- 2. org_unit_closure precomputes the transitive ancestor/descendant graph.
--    Access inheritance ("HR admin sees all HR sub-teams") is then a single
--    indexed lookup instead of a recursive CTE per request.

-- Shared trigger function reused by all tables that carry updated_at.
CREATE OR REPLACE FUNCTION set_updated_at()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN NEW.updated_at = now(); RETURN NEW; END;
$$;

-- ---------------------------------------------------------------------------
-- Companies
-- ---------------------------------------------------------------------------

CREATE TABLE companies (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    name        TEXT        NOT NULL,
    slug        TEXT        NOT NULL UNIQUE,
    settings    JSONB       NOT NULL DEFAULT '{}',  -- feature flags, branding, etc.
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TRIGGER companies_updated_at
    BEFORE UPDATE ON companies
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Org units
-- ---------------------------------------------------------------------------

CREATE TABLE org_units (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    parent_id   UUID        REFERENCES org_units(id) ON DELETE CASCADE,
    type        TEXT        NOT NULL
                            CHECK (type IN (
                                'subsidiary','division','department','team','project'
                            )),
    slug        TEXT        NOT NULL,
    name        TEXT        NOT NULL,
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Unique slug within (company, parent). NULL parent handled by sentinel UUID.
CREATE UNIQUE INDEX org_units_parent_slug_uidx
    ON org_units (
        company_id,
        COALESCE(parent_id, '00000000-0000-0000-0000-000000000000'),
        slug
    );

CREATE INDEX org_units_company_id_idx ON org_units (company_id);
CREATE INDEX org_units_parent_id_idx  ON org_units (parent_id);

CREATE TRIGGER org_units_updated_at
    BEFORE UPDATE ON org_units
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- Org unit closure table (transitive ancestor/descendant graph)
--
-- Maintained by trigger on insert. Deletions cascade automatically via FK.
--
-- Usage:
--   descendants: SELECT descendant_id FROM org_unit_closure WHERE ancestor_id   = $x
--   ancestors:   SELECT ancestor_id   FROM org_unit_closure WHERE descendant_id = $x
-- ---------------------------------------------------------------------------

CREATE TABLE org_unit_closure (
    ancestor_id   UUID NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    descendant_id UUID NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    company_id    UUID NOT NULL REFERENCES companies(id)  ON DELETE CASCADE,
    depth         INT  NOT NULL CHECK (depth >= 0),
    PRIMARY KEY (ancestor_id, descendant_id)
);

CREATE INDEX org_unit_closure_descendant_idx ON org_unit_closure (descendant_id);
CREATE INDEX org_unit_closure_company_idx    ON org_unit_closure (company_id, ancestor_id);

CREATE OR REPLACE FUNCTION org_unit_closure_on_insert()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    -- Self-entry (depth 0)
    INSERT INTO org_unit_closure (ancestor_id, descendant_id, company_id, depth)
    VALUES (NEW.id, NEW.id, NEW.company_id, 0);

    -- Inherit ancestor rows from the parent's existing closure entries
    IF NEW.parent_id IS NOT NULL THEN
        INSERT INTO org_unit_closure (ancestor_id, descendant_id, company_id, depth)
        SELECT c.ancestor_id, NEW.id, NEW.company_id, c.depth + 1
        FROM org_unit_closure c
        WHERE c.descendant_id = NEW.parent_id;
    END IF;

    RETURN NULL;
END;
$$;

CREATE TRIGGER org_units_closure_insert
    AFTER INSERT ON org_units
    FOR EACH ROW EXECUTE FUNCTION org_unit_closure_on_insert();
-- Deletions: CASCADE on both FK columns of org_unit_closure cleans up
-- automatically when any org_unit is deleted (subtrees cascade via
-- org_units.parent_id ON DELETE CASCADE too).
