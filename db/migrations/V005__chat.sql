-- Chat sessions and turns (RAG-powered Q&A)

CREATE TABLE chat_sessions (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID        NOT NULL REFERENCES orgs(id) ON DELETE CASCADE,
    user_id     UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title       TEXT,                           -- auto-generated from first question
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX chat_sessions_org_user_idx ON chat_sessions (org_id, user_id);
CREATE INDEX chat_sessions_updated_idx  ON chat_sessions (updated_at DESC);

CREATE TRIGGER chat_sessions_updated_at
    BEFORE UPDATE ON chat_sessions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE TABLE chat_turns (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    session_id      UUID        NOT NULL REFERENCES chat_sessions(id) ON DELETE CASCADE,
    question        TEXT        NOT NULL,
    answer          TEXT,
    -- Sources: array of {doc_id, filename, chunk_index, score, excerpt}
    rag_sources     JSONB       NOT NULL DEFAULT '[]',
    -- MCP tools invoked during this turn: array of {tool, args, result_summary}
    tool_calls      JSONB       NOT NULL DEFAULT '[]',
    -- Effective orgs searched for this turn (snapshot for audit)
    searched_orgs   UUID[]      NOT NULL DEFAULT '{}',
    tokens_used     INT,
    latency_ms      INT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX chat_turns_session_idx ON chat_turns (session_id, created_at);
