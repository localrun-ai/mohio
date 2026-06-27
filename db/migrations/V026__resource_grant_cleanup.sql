-- V026: AFTER DELETE triggers to clean up zombie resource_grants
--
-- resource_grants.resource_id and resource_grants.principal_id are
-- polymorphic columns (typed by resource_type / principal_type). PostgreSQL
-- cannot express a FK over a polymorphic column, so the V009 trigger
-- validates existence on INSERT/UPDATE only. The gap: when a referenced
-- resource is hard-deleted, the resource_grants rows pointing at it
-- become zombies - referencing nothing - until manually purged.
--
-- Not a security issue (the access resolver won't find anything to grant
-- access to), but it's debris that shows up oddly in admin "list grants"
-- queries and bloats the table over time.
--
-- Fix: AFTER DELETE triggers on each table that resource_grants can
-- point at (documents, wiki_pages, org_units) that delete the matching
-- resource_grants rows in the same transaction.
--
-- Notes:
--   * Company-level cascade is already handled by the existing
--     resource_grants.company_id FK ON DELETE CASCADE (V002), so we don't
--     need a trigger on companies.
--   * org_units triggers cover both directions: org_unit can be either a
--     resource (resource_type='org_unit') or a principal
--     (principal_type='org_unit'). Both flavours of zombie are cleaned.
--   * The DELETE from resource_grants will fire the V024 resource_grants
--     history trigger, which writes a 'delete' history row - so the
--     cleanup itself is auditable in the temporal history.
--   * users and groups are NOT covered. resource_grants.principal_type is
--     CHECK-restricted to 'org_unit' for MVP (V002), so users/groups can
--     never appear as principals in this table today. If/when the schema
--     is extended to allow principal_type IN ('user','group') (the
--     access_tokens TEXT[] model discussed in V002's header), add matching
--     AFTER DELETE triggers on users and groups so revocation cascades
--     remain symmetric. Until then, this is a deliberate non-issue.

-- ---------------------------------------------------------------------------
-- documents -> resource_grants
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION cleanup_resource_grants_on_document_delete()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM resource_grants
    WHERE company_id    = OLD.company_id
      AND resource_type = 'document'
      AND resource_id   = OLD.id;
    RETURN OLD;
END;
$$;

CREATE TRIGGER documents_cleanup_resource_grants
    AFTER DELETE ON documents
    FOR EACH ROW EXECUTE FUNCTION cleanup_resource_grants_on_document_delete();

-- ---------------------------------------------------------------------------
-- wiki_pages -> resource_grants
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION cleanup_resource_grants_on_wiki_page_delete()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM resource_grants
    WHERE company_id    = OLD.company_id
      AND resource_type = 'wiki_page'
      AND resource_id   = OLD.id;
    RETURN OLD;
END;
$$;

CREATE TRIGGER wiki_pages_cleanup_resource_grants
    AFTER DELETE ON wiki_pages
    FOR EACH ROW EXECUTE FUNCTION cleanup_resource_grants_on_wiki_page_delete();

-- ---------------------------------------------------------------------------
-- org_units -> resource_grants (both directions)
--
-- An org_unit can appear in resource_grants as:
--   * resource_id  (resource_type = 'org_unit')      - grant ON an org_unit
--   * principal_id (principal_type = 'org_unit')     - grant TO an org_unit
-- Both are cleaned in the same pass.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION cleanup_resource_grants_on_org_unit_delete()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM resource_grants
    WHERE company_id = OLD.company_id
      AND (
            (resource_type  = 'org_unit' AND resource_id  = OLD.id)
         OR (principal_type = 'org_unit' AND principal_id = OLD.id)
      );
    RETURN OLD;
END;
$$;

CREATE TRIGGER org_units_cleanup_resource_grants
    AFTER DELETE ON org_units
    FOR EACH ROW EXECUTE FUNCTION cleanup_resource_grants_on_org_unit_delete();
