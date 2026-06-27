-- V009: resource_grants same-company validation trigger
--
-- resource_grants uses polymorphic principal_id and resource_id columns that
-- PostgreSQL cannot enforce with foreign keys. This trigger validates that both
-- the principal and the resource exist and belong to the same company as the
-- grant row before every INSERT or UPDATE.
--
-- Must run after V003 (documents) and V004 (wiki_pages) because the trigger
-- function references those tables.

CREATE OR REPLACE FUNCTION validate_resource_grant_same_company()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    -- Validate principal exists in the correct table for its company.
    IF NEW.principal_type = 'user' THEN
        IF NOT EXISTS (
            SELECT 1 FROM users
            WHERE company_id = NEW.company_id AND id = NEW.principal_id
        ) THEN
            RAISE EXCEPTION 'resource_grants: user % not found in company %',
                NEW.principal_id, NEW.company_id;
        END IF;

    ELSIF NEW.principal_type = 'group' THEN
        IF NOT EXISTS (
            SELECT 1 FROM groups
            WHERE company_id = NEW.company_id AND id = NEW.principal_id
        ) THEN
            RAISE EXCEPTION 'resource_grants: group % not found in company %',
                NEW.principal_id, NEW.company_id;
        END IF;

    ELSIF NEW.principal_type = 'org_unit' THEN
        IF NOT EXISTS (
            SELECT 1 FROM org_units
            WHERE company_id = NEW.company_id AND id = NEW.principal_id
        ) THEN
            RAISE EXCEPTION 'resource_grants: org_unit % not found in company %',
                NEW.principal_id, NEW.company_id;
        END IF;
    END IF;

    -- Validate resource exists in the correct table for its company.
    IF NEW.resource_type = 'org_unit' THEN
        IF NOT EXISTS (
            SELECT 1 FROM org_units
            WHERE company_id = NEW.company_id AND id = NEW.resource_id
        ) THEN
            RAISE EXCEPTION 'resource_grants: org_unit resource % not found in company %',
                NEW.resource_id, NEW.company_id;
        END IF;

    ELSIF NEW.resource_type = 'document' THEN
        IF NOT EXISTS (
            SELECT 1 FROM documents
            WHERE company_id = NEW.company_id AND id = NEW.resource_id
        ) THEN
            RAISE EXCEPTION 'resource_grants: document % not found in company %',
                NEW.resource_id, NEW.company_id;
        END IF;

    ELSIF NEW.resource_type = 'wiki_page' THEN
        IF NOT EXISTS (
            SELECT 1 FROM wiki_pages
            WHERE company_id = NEW.company_id AND id = NEW.resource_id
        ) THEN
            RAISE EXCEPTION 'resource_grants: wiki_page % not found in company %',
                NEW.resource_id, NEW.company_id;
        END IF;
    END IF;

    RETURN NEW;
END;
$$;

CREATE TRIGGER resource_grants_validate_same_company
    BEFORE INSERT OR UPDATE ON resource_grants
    FOR EACH ROW
    EXECUTE FUNCTION validate_resource_grant_same_company();
