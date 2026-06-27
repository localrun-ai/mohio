-- V001: Companies and org units
--
-- Every company gets one automatic root org_unit (type='root', parent_id=NULL,
-- slug='root', name=company.name). All other org units are children of root or
-- deeper descendants. This keeps access scope resolution uniform: resolving
-- "what can user X see?" is always a single closure-table lookup regardless of
-- whether the resource is company-wide or scoped to a team.
--
-- Root does NOT imply "can read everything". It is the company-wide scope.
-- Membership semantics:
--   membership on root, applies_to=self_only         => company-wide docs only
--   membership on root, applies_to=self_and_descendants => full company tree
--   membership on HR,   applies_to=self_and_descendants => HR subtree only
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
--
-- type='root': one per company, parent_id=NULL, slug='root', name=company name.
-- All other types must have a non-NULL parent_id (enforced by CHECK).
-- C++ should identify the root by: type='root' AND company_id=X
-- (or cache the root_org_unit_id on startup).
-- ---------------------------------------------------------------------------

CREATE TABLE org_units (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    parent_id   UUID,                            -- NULL only for type='root'
    type        TEXT        NOT NULL
                            CHECK (type IN (
                                'root',
                                'subsidiary','division','department','team','project'
                            )),
    slug        TEXT        NOT NULL,
    name        TEXT        NOT NULL,
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- Required so composite FKs from other tables can reference (company_id, id).
    UNIQUE (company_id, id),

    -- root <=> parent_id IS NULL. All other units must have a parent.
    CONSTRAINT org_units_root_parent_shape_chk
        CHECK (
            (type = 'root' AND parent_id IS NULL)
            OR
            (type <> 'root' AND parent_id IS NOT NULL)
        ),

    -- Composite self-reference: enforces parent belongs to the same company.
    -- NULL parent_id (root units) is not checked by PostgreSQL FK, which is
    -- correct: root has no parent.
    CONSTRAINT org_units_parent_same_company_fk
        FOREIGN KEY (company_id, parent_id)
        REFERENCES org_units(company_id, id)
        ON DELETE CASCADE
);

-- Unique slug within the same parent (NULL parent uses zero UUID as sentinel).
CREATE UNIQUE INDEX org_units_parent_slug_uidx
    ON org_units (
        company_id,
        COALESCE(parent_id, '00000000-0000-0000-0000-000000000000'),
        slug
    );

-- At most one root per company.
CREATE UNIQUE INDEX org_units_one_root_per_company_uidx
    ON org_units (company_id)
    WHERE type = 'root';

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

-- Ancestor -> descendants: "give me all org_units under X in company C"
CREATE INDEX org_unit_closure_company_ancestor_idx
    ON org_unit_closure (company_id, ancestor_id, descendant_id);
-- Descendant -> ancestors: "give me all ancestors of X in company C" (access scope resolution)
CREATE INDEX org_unit_closure_company_descendant_idx
    ON org_unit_closure (company_id, descendant_id, ancestor_id);

CREATE OR REPLACE FUNCTION org_unit_closure_on_insert()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    -- Self-entry (depth 0)
    INSERT INTO org_unit_closure (company_id, ancestor_id, descendant_id, depth)
    VALUES (NEW.company_id, NEW.id, NEW.id, 0);

    -- Inherit all ancestor rows from the parent.
    -- company_id filter is redundant given the composite FK, but it lets the
    -- planner use org_unit_closure_company_descendant_idx instead of scanning
    -- by descendant_id alone.
    IF NEW.parent_id IS NOT NULL THEN
        INSERT INTO org_unit_closure (company_id, ancestor_id, descendant_id, depth)
        SELECT NEW.company_id, c.ancestor_id, NEW.id, c.depth + 1
        FROM org_unit_closure c
        WHERE c.company_id    = NEW.company_id
          AND c.descendant_id = NEW.parent_id;
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

-- ---------------------------------------------------------------------------
-- Auto-create root org_unit when a company is inserted.
-- The org_units_closure_insert trigger fires immediately after, giving root
-- a self-entry at depth 0 in org_unit_closure.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION create_root_org_unit_for_company()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO org_units (company_id, parent_id, type, name, slug)
    VALUES (NEW.id, NULL, 'root', NEW.name, 'root');
    RETURN NEW;
END;
$$;

CREATE TRIGGER companies_create_root_org_unit
    AFTER INSERT ON companies
    FOR EACH ROW EXECUTE FUNCTION create_root_org_unit_for_company();

-- ---------------------------------------------------------------------------
-- Prevent manual deletion of the root org_unit.
-- Company-cascade deletes are allowed: when the company row is already gone
-- (i.e., this delete was triggered by ON DELETE CASCADE), EXISTS returns false
-- and the delete proceeds. A direct DELETE on the root unit finds the company
-- still present and raises.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION prevent_root_org_unit_delete()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF OLD.type = 'root' THEN
        IF EXISTS (SELECT 1 FROM companies WHERE id = OLD.company_id) THEN
            RAISE EXCEPTION 'Cannot delete root org_unit for company %', OLD.company_id;
        END IF;
    END IF;
    RETURN OLD;
END;
$$;

CREATE TRIGGER org_units_prevent_root_delete
    BEFORE DELETE ON org_units
    FOR EACH ROW EXECUTE FUNCTION prevent_root_org_unit_delete();

-- ---------------------------------------------------------------------------
-- Keep root org_unit name in sync with company name.
-- updated_at is handled by the existing org_units_updated_at BEFORE UPDATE
-- trigger; no need to set it explicitly here.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION sync_root_org_unit_name()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF OLD.name IS DISTINCT FROM NEW.name THEN
        UPDATE org_units
        SET name = NEW.name
        WHERE company_id = NEW.id AND type = 'root';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER companies_sync_root_org_unit_name
    AFTER UPDATE OF name ON companies
    FOR EACH ROW EXECUTE FUNCTION sync_root_org_unit_name();
