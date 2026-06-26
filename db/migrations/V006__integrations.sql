-- External platform integrations (MCP tools: Slack, Jira, Confluence, etc.)
-- credentials column holds AES-256-GCM encrypted JSON (key managed via env var).

CREATE TABLE integrations (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    type        TEXT        NOT NULL CHECK (type IN (
                                'slack','jira','confluence',
                                'github','gitlab',
                                'm365','google_workspace',
                                'webhook')),
    name        TEXT        NOT NULL,
    base_url    TEXT,                           -- for self-hosted Jira/Confluence/GitLab
    credentials TEXT,                           -- AES-256-GCM encrypted JSON
    enabled     BOOLEAN     NOT NULL DEFAULT true,
    created_by  UUID        REFERENCES users(id),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, type, name)
);

CREATE INDEX integrations_org_id_idx ON integrations (org_id);

CREATE TRIGGER integrations_updated_at
    BEFORE UPDATE ON integrations
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- Individual MCP tools exposed per integration.
-- tool_name examples: slack_search, slack_post, jira_search,
--   jira_create_issue, confluence_search, github_search_code, webhook_call
CREATE TABLE mcp_tools (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    integration_id  UUID        NOT NULL REFERENCES integrations(id) ON DELETE CASCADE,
    tool_name       TEXT        NOT NULL,
    description     TEXT,
    -- JSON Schema for the tool's input args (forwarded to LLM as tool definition)
    input_schema    JSONB       NOT NULL DEFAULT '{}',
    -- read=true: results may be cached in Redis. false: always live (e.g. post/create)
    read_only       BOOLEAN     NOT NULL DEFAULT true,
    enabled         BOOLEAN     NOT NULL DEFAULT true,
    config          JSONB       NOT NULL DEFAULT '{}',
    UNIQUE (integration_id, tool_name)
);

CREATE INDEX mcp_tools_integration_idx ON mcp_tools (integration_id);
