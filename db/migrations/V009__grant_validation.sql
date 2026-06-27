-- V009: resource_grants same-company validation trigger
--
-- resource_grants.resource_id is polymorphic (org_unit | document | wiki_page)
-- and cannot be enforced with a FK. This trigger validates that the resource
-- exists in the correct table and belongs to the same company as the grant row.
--
-- principal_type is restricted to 'org_unit' by CHECK in V002 (MVP). When
-- user/group-specific grants are added (access_tokens model), extend this
-- trigger to validate those principal types.
--
-- Must run after V003 (documents) and V004 (wiki_pages).

CREATE OR REPLACE FUNCTION validate_resource_grant_same_company()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    -- Validate principal org_unit exists in this company.
    IF NOT EXISTS (
        SELECT 1 FROM org_units
        WHERE company_id = NEW.company_id AND id = NEW.principal_id
    ) THEN
        RAISE EXCEPTION 'resource_grants: org_unit % not found in company %',
            NEW.principal_id, NEW.company_id;
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
