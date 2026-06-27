-- V006: External integrations and MCP tools
--
-- Integrations are scoped to an org_unit. Composite FK enforces that the
-- org_unit belongs to the same company as the integration record.
--
-- credentials: AES-256-GCM encrypted JSON blob (base64). Encryption key
-- lives in the CREDENTIALS_KEY environment variable, never in the DB.
--
-- MCP tools are the LLM-callable interface per integration.
-- read_only=true  -> results may be cached in Redis (e.g. jira_search)
-- read_only=false -> always executed live, never cached (e.g. slack_post)

CREATE TABLE integrations (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL,
    org_unit_id UUID        NOT NULL,
    type        TEXT        NOT NULL CHECK (type IN (
                                'slack', 'jira', 'confluence',
                                'github', 'gitlab',
                                'm365', 'sharepoint', 'google_workspace',
                                'webhook'
                            )),
    name        TEXT        NOT NULL,
    base_url    TEXT,                             -- self-hosted Jira/Confluence/GitLab
    credentials TEXT,                             -- AES-256-GCM encrypted JSON blob
    enabled     BOOLEAN     NOT NULL DEFAULT true,
    created_by  UUID        REFERENCES users(id) ON DELETE SET NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_unit_id, type, name),
    CONSTRAINT integrations_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE
);

CREATE INDEX integrations_org_unit_idx ON integrations (org_unit_id);
CREATE INDEX integrations_company_idx  ON integrations (company_id);

CREATE TRIGGER integrations_updated_at
    BEFORE UPDATE ON integrations
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE OR REPLACE FUNCTION validate_integrations_actors()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM validate_actor_same_company(NEW.created_by, NEW.company_id, 'created_by');
    RETURN NEW;
END;
$$;

CREATE TRIGGER integrations_validate_actors
    BEFORE INSERT OR UPDATE ON integrations
    FOR EACH ROW EXECUTE FUNCTION validate_integrations_actors();

CREATE TABLE mcp_tools (
    id             UUID    PRIMARY KEY DEFAULT gen_random_uuid(),
    integration_id UUID    NOT NULL REFERENCES integrations(id) ON DELETE CASCADE,
    tool_name      TEXT    NOT NULL,
    description    TEXT,
    input_schema   JSONB   NOT NULL DEFAULT '{}',  -- JSON Schema forwarded to LLM
    read_only      BOOLEAN NOT NULL DEFAULT true,
    enabled        BOOLEAN NOT NULL DEFAULT true,
    config         JSONB   NOT NULL DEFAULT '{}',
    UNIQUE (integration_id, tool_name)
);

CREATE INDEX mcp_tools_integration_idx ON mcp_tools (integration_id);
