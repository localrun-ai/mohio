-- V023: shared_chat_threads with frozen access scope snapshot (K8)
--
-- Per review_feedback_v2.md K8: shared chats with no scope snapshot leak
-- in one of two ways - either the recipient sees content the original
-- asker could but the recipient cannot (security bug), or the chat
-- re-runs retrieval under the recipient's scope and the cited chunks
-- disappear (broken UX). Retrofitting is risky because old shares
-- predate the snapshot.
--
-- Why now: the share button doesn't have to ship in iteration 3, but
-- the table needs to exist so the snapshot column is populated from the
-- first share. Adding the snapshot after some shares already exist
-- forces a manual "what scope did Alice have on date D?" backfill - and
-- without temporal access history, that backfill is best-effort.
--
-- Design:
--   * access_scope_snapshot is an immutable UUID[] of the original
--     asker's resolved org_unit set at the moment of sharing. The chat
--     replay path filters retrieved chunks by intersection with this
--     snapshot, NOT with the recipient's current scope.
--   * shared_by_user_id is the asker (or a delegated admin). NOT the
--     recipient - there can be many recipients; the share is fanned out
--     via revoked_at / link-based access.
--   * revoked_at: null = active. Once set, replay returns "share
--     revoked" without exposing any further content.
--   * Composite FK to chat_sessions enforces same-company. The chat
--     session in turn pins the asker (chat_sessions.user_id).
--   * No FK to specific chat turns - shares apply at the session level.
--     Per-turn sharing is a future extension if needed.

CREATE TABLE shared_chat_threads (
    id                     UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id             UUID        NOT NULL,
    chat_session_id        UUID        NOT NULL,
    shared_by_user_id      UUID        NOT NULL,
    -- The original asker's resolved access scope at sharing time.
    -- Replay filters citations by intersection with this snapshot,
    -- never the recipient's current scope.
    access_scope_snapshot  UUID[]      NOT NULL,
    -- Token / slug used in the share URL. Indexed UNIQUE for direct
    -- lookup; not a security boundary on its own (recipient still
    -- needs to authenticate, and the snapshot bounds what they see).
    share_token            TEXT        NOT NULL,
    shared_at              TIMESTAMPTZ NOT NULL DEFAULT now(),
    revoked_at             TIMESTAMPTZ,
    revoked_by_user_id     UUID,                       -- nullable; SET NULL on user delete

    UNIQUE (share_token),

    CONSTRAINT shared_chat_session_same_company_fk
        FOREIGN KEY (company_id, chat_session_id)
        REFERENCES chat_sessions(company_id, id) ON DELETE CASCADE,

    CONSTRAINT shared_chat_sharer_same_company_fk
        FOREIGN KEY (company_id, shared_by_user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE,

    CONSTRAINT shared_chat_revoker_fk
        FOREIGN KEY (revoked_by_user_id)
        REFERENCES users(id) ON DELETE SET NULL,

    -- revoked_at presence and revoked_by_user_id presence move together.
    CONSTRAINT shared_chat_revoke_consistent_chk
        CHECK ((revoked_at IS NULL) = (revoked_by_user_id IS NULL))
);

-- Replay path: "is this share still active?" lookup by token.
CREATE INDEX shared_chat_active_by_token_idx
    ON shared_chat_threads (share_token)
    WHERE revoked_at IS NULL;

-- Admin: "all shares for a chat session" or "all active shares in company X."
CREATE INDEX shared_chat_session_idx
    ON shared_chat_threads (company_id, chat_session_id);

CREATE INDEX shared_chat_company_active_idx
    ON shared_chat_threads (company_id, shared_at DESC)
    WHERE revoked_at IS NULL;

-- ---------------------------------------------------------------------------
-- Immutability of the snapshot: access_scope_snapshot must never change
-- after the row is created. The whole point of the snapshot is that the
-- share has a fixed-at-creation view of what the original asker could
-- see. Mutating it would silently re-scope every existing share.
-- ---------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION prevent_shared_chat_snapshot_mutation()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF OLD.access_scope_snapshot IS DISTINCT FROM NEW.access_scope_snapshot
       OR OLD.chat_session_id     IS DISTINCT FROM NEW.chat_session_id
       OR OLD.shared_by_user_id   IS DISTINCT FROM NEW.shared_by_user_id
       OR OLD.shared_at           IS DISTINCT FROM NEW.shared_at
       OR OLD.company_id          IS DISTINCT FROM NEW.company_id THEN
        RAISE EXCEPTION 'shared_chat_threads snapshot/session/sharer/company are immutable; '
                        'revoke and re-share instead';
    END IF;
    RETURN NEW;
END;
$$;

CREATE TRIGGER shared_chat_threads_immutable
    BEFORE UPDATE ON shared_chat_threads
    FOR EACH ROW EXECUTE FUNCTION prevent_shared_chat_snapshot_mutation();
