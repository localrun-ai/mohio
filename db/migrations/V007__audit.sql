-- V007: Audit log (append-only, quarterly partitions)
--
-- Records every security-relevant action with enough context to answer:
-- who did what to which resource, when, and from where.
--
-- company_id / user_id / org_unit_id are plain UUIDs with NO foreign keys.
-- Reasons:
--   1. FK checks acquire a lock on the referenced table on every INSERT,
--      adding contention on the hot path.
--   2. ON DELETE SET NULL would null out the actor after a user is deleted,
--      making the row unattributable - exactly the opposite of what audit needs.
--
-- Callers MUST denormalize identity into detail at write time, e.g.:
--   { "user_email": "alice@acme.com", "org_unit_name": "HR",
--     "resource_type": "document", "resource_id": "<uuid>" }
-- This preserves the full audit trail even after the referenced entity is gone.
--
-- Partitioned by created_at quarter to allow archiving old data without
-- locking or vacuuming the live table.

CREATE TABLE audit_log (
    id          BIGSERIAL,
    company_id  UUID,                             -- no FK; see header comment
    user_id     UUID,                             -- no FK; caller puts email in detail
    org_unit_id UUID,                             -- no FK; caller puts name in detail
    action      TEXT        NOT NULL,
    -- Action namespaces:
    --   auth:    login, logout, token_revoke
    --   company: company_create, company_update
    --   org:     org_unit_create, org_unit_update, org_unit_delete
    --   member:  member_add, member_remove, member_role_change
    --   grant:   grant_create, grant_revoke
    --   doc:     doc_ingest, doc_activate, doc_deprecate, doc_archive, doc_delete
    --   chunk:   chunk_scope_update   (Qdrant payload resync)
    --   chat:    chat_query
    --   wiki:    wiki_create, wiki_edit, wiki_activate, wiki_archive, wiki_query, wiki_lint
    --   mcp:     tool_call
    --   apikey:  apikey_create, apikey_revoke
    detail      JSONB       NOT NULL DEFAULT '{}',
    ip_addr     INET,
    user_agent  TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (id, created_at)
) PARTITION BY RANGE (created_at);

CREATE TABLE audit_log_2026_q2 PARTITION OF audit_log
    FOR VALUES FROM ('2026-04-01') TO ('2026-07-01');
CREATE TABLE audit_log_2026_q3 PARTITION OF audit_log
    FOR VALUES FROM ('2026-07-01') TO ('2026-10-01');
CREATE TABLE audit_log_2026_q4 PARTITION OF audit_log
    FOR VALUES FROM ('2026-10-01') TO ('2027-01-01');
CREATE TABLE audit_log_2027_q1 PARTITION OF audit_log
    FOR VALUES FROM ('2027-01-01') TO ('2027-04-01');
CREATE TABLE audit_log_2027_q2 PARTITION OF audit_log
    FOR VALUES FROM ('2027-04-01') TO ('2027-07-01');

-- Catch-all for any rows that fall outside the explicit quarterly partitions.
-- Prevents INSERT failures when a new quarter arrives before the partition
-- is manually added. Rows here should be moved to a named partition promptly.
CREATE TABLE audit_log_default PARTITION OF audit_log DEFAULT;

CREATE INDEX audit_log_company_idx ON audit_log (company_id,  created_at DESC);
CREATE INDEX audit_log_user_idx    ON audit_log (user_id,     created_at DESC);
CREATE INDEX audit_log_org_idx     ON audit_log (org_unit_id, created_at DESC);
CREATE INDEX audit_log_action_idx  ON audit_log (action,      created_at DESC);
