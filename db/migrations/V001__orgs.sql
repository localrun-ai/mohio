-- Organisation hierarchy (self-referencing tree)
-- Supports: company -> subsidiary -> division -> department -> team
-- subsidiary and division are optional levels.

CREATE TABLE orgs (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    parent_id   UUID        REFERENCES orgs(id) ON DELETE CASCADE,
    type        TEXT        NOT NULL
                            CHECK (type IN ('company','subsidiary','division','department','team')),
    slug        TEXT        NOT NULL,
    name        TEXT        NOT NULL,
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Unique slug within the same parent (NULLs handled via coalesce sentinel)
CREATE UNIQUE INDEX orgs_parent_slug_uidx
    ON orgs (COALESCE(parent_id, '00000000-0000-0000-0000-000000000000'), slug);

-- Fast upward tree traversal
CREATE INDEX orgs_parent_id_idx ON orgs (parent_id);

-- Trigger: keep updated_at current
CREATE OR REPLACE FUNCTION set_updated_at()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN NEW.updated_at = now(); RETURN NEW; END;
$$;

CREATE TRIGGER orgs_updated_at
    BEFORE UPDATE ON orgs
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- Helper view: full ancestry path as array (uses recursive CTE)
CREATE VIEW org_ancestors AS
WITH RECURSIVE anc(id, ancestor_id, depth) AS (
    SELECT id, parent_id, 1 FROM orgs WHERE parent_id IS NOT NULL
    UNION ALL
    SELECT anc.id, orgs.parent_id, anc.depth + 1
    FROM anc JOIN orgs ON orgs.id = anc.ancestor_id
    WHERE orgs.parent_id IS NOT NULL
)
SELECT id, array_agg(ancestor_id ORDER BY depth) AS ancestors
FROM anc
GROUP BY id;
