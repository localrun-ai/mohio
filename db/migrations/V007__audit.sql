-- Immutable audit trail. Never updated or deleted (append-only).
-- Partitioned by month so large deployments can archive/drop old partitions.

CREATE TABLE audit_log (
    id          BIGSERIAL,
    user_id     UUID        REFERENCES users(id),
    org_id      UUID        REFERENCES orgs(id),
    action      TEXT        NOT NULL,
    -- action values:
    --   auth:    login, logout, token_revoke
    --   org:     org_create, org_update, org_delete
    --   member:  member_add, member_remove, member_role_change
    --   grant:   access_grant, access_revoke
    --   doc:     doc_ingest, doc_delete
    --   chat:    chat_query
    --   wiki:    wiki_edit, wiki_delete, wiki_lint, wiki_query
    --   mcp:     tool_call
    --   apikey:  apikey_create, apikey_revoke
    detail      JSONB       NOT NULL DEFAULT '{}',
    ip_addr     INET,
    user_agent  TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (id, created_at)
) PARTITION BY RANGE (created_at);

-- Create initial partitions (bootstrap; add more via scheduled job or migration)
CREATE TABLE audit_log_2026_q3 PARTITION OF audit_log
    FOR VALUES FROM ('2026-07-01') TO ('2026-10-01');

CREATE TABLE audit_log_2026_q4 PARTITION OF audit_log
    FOR VALUES FROM ('2026-10-01') TO ('2027-01-01');

CREATE TABLE audit_log_2027_q1 PARTITION OF audit_log
    FOR VALUES FROM ('2027-01-01') TO ('2027-04-01');

CREATE INDEX audit_log_user_idx   ON audit_log (user_id, created_at DESC);
CREATE INDEX audit_log_org_idx    ON audit_log (org_id,  created_at DESC);
CREATE INDEX audit_log_action_idx ON audit_log (action,  created_at DESC);
