-- V005: Chat sessions and turns
--
-- Sessions are scoped to an org_unit. Composite FKs enforce that both the
-- org_unit and the user belong to the same company as the session.
--
-- access_scope_ids on each turn is an immutable snapshot of the org_unit IDs
-- that were searched, forming an audit record of what evidence was eligible
-- regardless of permission changes after the fact.

CREATE TABLE chat_sessions (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id  UUID        NOT NULL,
    org_unit_id UUID        NOT NULL,
    user_id     UUID        NOT NULL,
    title       TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Required target for composite FK from chat_turns.
    UNIQUE (company_id, id),
    CONSTRAINT chat_sessions_org_unit_same_company_fk
        FOREIGN KEY (company_id, org_unit_id)
        REFERENCES org_units(company_id, id) ON DELETE CASCADE,
    CONSTRAINT chat_sessions_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE
);

CREATE INDEX chat_sessions_company_org_user_idx ON chat_sessions (company_id, org_unit_id, user_id);
CREATE INDEX chat_sessions_updated_idx          ON chat_sessions (updated_at DESC);

CREATE TRIGGER chat_sessions_updated_at
    BEFORE UPDATE ON chat_sessions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE TABLE chat_turns (
    id               UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id       UUID        NOT NULL,
    session_id       UUID        NOT NULL,
    question         TEXT        NOT NULL,
    answer           TEXT,
    -- [{doc_id, version_id, chunk_id, score, excerpt}] - chunks shown to the LLM.
    -- version_id pins the citation to the exact document_versions row so the
    -- evidence is reproducible even after the document is superseded.
    rag_sources      JSONB       NOT NULL DEFAULT '[]',
    -- [{tool, args, result_summary}]
    tool_calls       JSONB       NOT NULL DEFAULT '[]',
    -- Immutable snapshot: which org_unit IDs were searched for this query.
    access_scope_ids UUID[]      NOT NULL DEFAULT '{}',
    tokens_used      INT         CHECK (tokens_used IS NULL OR tokens_used >= 0),
    latency_ms       INT         CHECK (latency_ms  IS NULL OR latency_ms  >= 0),
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT chat_turns_session_same_company_fk
        FOREIGN KEY (company_id, session_id)
        REFERENCES chat_sessions(company_id, id) ON DELETE CASCADE
);

CREATE INDEX chat_turns_company_session_created_idx ON chat_turns (company_id, session_id, created_at);
