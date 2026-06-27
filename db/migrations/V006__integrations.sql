-- V006: External integrations and MCP tools
--
-- Integrations connect org_units to external platforms (Slack, Jira, etc.).
-- Credentials are stored AES-256-GCM encrypted; the key lives in env var.
--
-- MCP tools are the callable interface exposed to the LLM.
-- read_only=true means results can be cached in Redis (e.g. jira_search).
-- read_only=false means the call is always live and never cached (e.g. slack_post).

CREATE TABLE integrations (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id      UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    org_unit_id     UUID        NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    type            TEXT        NOT NULL CHECK (type IN (
                                    'slack', 'jira', 'confluence',
                                    'github', 'gitlab',
                                    'm365', 'sharepoint', 'google_workspace',
                                    'webhook'
                                )),
    name            TEXT        NOT NULL,
    base_url        TEXT,                           -- for self-hosted Jira/Confluence/GitLab
    credentials     TEXT,                           -- AES-256-GCM encrypted JSON blob
    enabled         BOOLEAN     NOT NULL DEFAULT true,
    created_by      UUID        REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_unit_id, type, name)
);

CREATE INDEX integrations_org_unit_idx ON integrations (org_unit_id);
CREATE INDEX integrations_company_idx  ON integrations (company_id);

CREATE TRIGGER integrations_updated_at
    BEFORE UPDATE ON integrations
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE TABLE mcp_tools (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    integration_id  UUID        NOT NULL REFERENCES integrations(id) ON DELETE CASCADE,
    tool_name       TEXT        NOT NULL,
    description     TEXT,
    -- JSON Schema forwarded to the LLM as the tool's input spec
    input_schema    JSONB       NOT NULL DEFAULT '{}',
    read_only       BOOLEAN     NOT NULL DEFAULT true,
    enabled         BOOLEAN     NOT NULL DEFAULT true,
    config          JSONB       NOT NULL DEFAULT '{}',
    UNIQUE (integration_id, tool_name)
);

CREATE INDEX mcp_tools_integration_idx ON mcp_tools (integration_id);
