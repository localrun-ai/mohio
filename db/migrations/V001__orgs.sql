-- V001: Companies and org units
--
-- Org units form a tree within a company:
--   [subsidiary] -> [division] -> department -> [team | project]
--
-- Cross-company integrity strategy:
--   Every table that belongs to a company carries an explicit company_id column.
--   Instead of simple UUID foreign keys (which only check existence), we use
--   COMPOSITE foreign keys of the form:
--
--     FOREIGN KEY (company_id, other_id) REFERENCES target(company_id, id)
--
--   This makes the database enforce same-company membership at every INSERT and
--   UPDATE, with no triggers or application-level checks required.
--
--   For each referenced table, a UNIQUE (company_id, id) constraint is declared
--   so PostgreSQL can use it as the target of composite FKs.
--
-- org_unit_closure precomputes the transitive ancestor/descendant graph so
-- access scope resolution ("which org units can user X read?") is a single
-- indexed lookup rather than a recursive CTE per request.

CREATE OR REPLACE FUNCTION set_updated_at()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN NEW.updated_at = now(); RETURN NEW; END;
$$;

-- ---------------------------------------------------------------------------
-- Companies (hard tenant boundary)
-- ---------------------------------------------------------------------------

CREATE TABLE companies (
    id         UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    name       TEXT        NOT NULL,
    slug       TEXT        NOT NULL UNIQUE,
    settings   JSONB       NOT NULL DEFAULT '{}',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
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
    parent_id   UUID,                            -- NULL = top-level unit for this company
    type        TEXT        NOT NULL
                            CHECK (type IN (
                                'subsidiary','division','department','team','project'
                            )),
    slug        TEXT        NOT NULL,
    name        TEXT        NOT NULL,
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Required so composite FKs from other tables can reference (company_id, id).
    UNIQUE (company_id, id),

    -- Composite self-reference: enforces parent belongs to the same company.
    -- NULL parent_id is not checked by PostgreSQL FK (NULL = no reference), which
    -- is correct: top-level units have no parent.
    CONSTRAINT org_units_parent_same_company_fk
        FOREIGN KEY (company_id, parent_id)
        REFERENCES org_units(company_id, id)
        ON DELETE CASCADE
);

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
-- Org unit closure (transitive ancestor/descendant graph)
--
-- Maintained by trigger on insert; CASCADE handles deletions.
--
-- Composite FKs enforce that ancestor and descendant both belong to the same
-- company as the closure row itself.
--
-- Queries:
--   Descendants of X:  SELECT descendant_id FROM org_unit_closure WHERE ancestor_id   = $x
--   Ancestors of X:    SELECT ancestor_id   FROM org_unit_closure WHERE descendant_id = $x
-- ---------------------------------------------------------------------------

CREATE TABLE org_unit_closure (
    company_id    UUID NOT NULL REFERENCES companies(id)  ON DELETE CASCADE,
    ancestor_id   UUID NOT NULL,
    descendant_id UUID NOT NULL,
    depth         INT  NOT NULL CHECK (depth >= 0),
    PRIMARY KEY (ancestor_id, descendant_id),
    CONSTRAINT closure_ancestor_same_company_fk
        FOREIGN KEY (company_id, ancestor_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE,
    CONSTRAINT closure_descendant_same_company_fk
        FOREIGN KEY (company_id, descendant_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

CREATE INDEX org_unit_closure_descendant_idx ON org_unit_closure (descendant_id);
CREATE INDEX org_unit_closure_company_idx    ON org_unit_closure (company_id, ancestor_id);

CREATE OR REPLACE FUNCTION org_unit_closure_on_insert()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    -- Self-entry (depth 0)
    INSERT INTO org_unit_closure (company_id, ancestor_id, descendant_id, depth)
    VALUES (NEW.company_id, NEW.id, NEW.id, 0);

    -- Inherit all ancestor rows from the parent
    IF NEW.parent_id IS NOT NULL THEN
        INSERT INTO org_unit_closure (company_id, ancestor_id, descendant_id, depth)
        SELECT NEW.company_id, c.ancestor_id, NEW.id, c.depth + 1
        FROM org_unit_closure c
        WHERE c.descendant_id = NEW.parent_id;
    END IF;

    RETURN NULL;
END;
$$;

CREATE TRIGGER org_units_closure_insert
    AFTER INSERT ON org_units
    FOR EACH ROW EXECUTE FUNCTION org_unit_closure_on_insert();

-- ---------------------------------------------------------------------------
-- Block direct parent_id updates (closure table would become stale).
-- Use move_org_unit() once implemented. Option A per design decision.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION prevent_org_unit_parent_update()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF OLD.parent_id IS DISTINCT FROM NEW.parent_id THEN
        RAISE EXCEPTION 'org_unit parent_id cannot be updated directly; use move_org_unit()';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER org_units_prevent_parent_update
    BEFORE UPDATE ON org_units
    FOR EACH ROW EXECUTE FUNCTION prevent_org_unit_parent_update();
