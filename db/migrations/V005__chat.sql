-- V005: Chat sessions and turns
--
-- RAG-powered Q&A sessions scoped to an org_unit.
-- access_scope_ids on each turn is a snapshot of the org_unit IDs that were
-- actually searched, providing an immutable audit record of what evidence
-- was eligible for that query (even if permissions change later).

CREATE TABLE chat_sessions (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    org_unit_id UUID        NOT NULL REFERENCES org_units(id) ON DELETE CASCADE,
    user_id     UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title       TEXT,                           -- auto-generated from first question
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX chat_sessions_org_user_idx ON chat_sessions (org_unit_id, user_id);
CREATE INDEX chat_sessions_company_idx  ON chat_sessions (company_id);
CREATE INDEX chat_sessions_updated_idx  ON chat_sessions (updated_at DESC);

CREATE TRIGGER chat_sessions_updated_at
    BEFORE UPDATE ON chat_sessions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE TABLE chat_turns (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    session_id          UUID        NOT NULL REFERENCES chat_sessions(id) ON DELETE CASCADE,
    question            TEXT        NOT NULL,
    answer              TEXT,
    -- Chunks retrieved and shown to the LLM: [{doc_id, chunk_id, score, excerpt}]
    rag_sources         JSONB       NOT NULL DEFAULT '[]',
    -- MCP tool calls made during this turn: [{tool, args, result_summary}]
    tool_calls          JSONB       NOT NULL DEFAULT '[]',
    -- Snapshot of access_scope_ids resolved for this user+org_unit at query time.
    -- Immutable after creation - used for auditing what was eligible to be retrieved.
    access_scope_ids    UUID[]      NOT NULL DEFAULT '{}',
    tokens_used         INT,
    latency_ms          INT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX chat_turns_session_idx ON chat_turns (session_id, created_at);
