-- V018: Chat turn + chunk feedback signals (K6)
--
-- Per the V2 schema review §K6 (review_feedback_v2.md): "Costs nothing now;
-- impossible to recover later." Once chats happen without feedback being
-- captured, the training signal for "which retrievals actually worked" is
-- gone forever. Adding the schema before iteration 3 ships the chat path
-- means the UI can attach thumbs-up/thumbs-down whenever it's ready and
-- the historical record is intact from day one.
--
-- Two tables:
--
--   chat_turn_feedback : one row per (turn, user) explicit signal.
--     User-attributable. UNIQUE(chat_turn_id, user_id) means a user
--     changing their mind UPDATEs the row rather than INSERTing a second.
--     Signal is SMALLINT CHECK IN (-1, +1); a cleared vote DELETEs.
--
--   chunk_quality_signals : aggregated counters per chunk, maintained
--     by application/worker from chat_turn_feedback joined with
--     chat_turns.rag_sources. Lets retrieval downweight chunks with
--     consistent negative feedback without scanning the full feedback
--     table on the hot path.
--
-- The aggregator is intentionally NOT a trigger:
--   * Trigger contention on the hot chat-write path is unacceptable.
--   * The rag_sources JSONB → chunk_id list expansion is non-trivial
--     and better expressed in application code.
--   * The table sits empty until a scheduler job materializes it.
--
-- A prerequisite ALTER on chat_turns adds UNIQUE (company_id, id) so
-- chat_turn_feedback can use a composite FK that enforces same-company
-- membership at the DB level — consistent with the rest of the schema.

-- ---------------------------------------------------------------------------
-- chat_turns: expose (company_id, id) for composite FK targets.
--
-- id is already a PRIMARY KEY (unique by definition); this constraint is
-- redundant for uniqueness but required as an FK target. Cheap.
-- ---------------------------------------------------------------------------

ALTER TABLE chat_turns
    ADD CONSTRAINT chat_turns_company_id_uniq UNIQUE (company_id, id);

-- ---------------------------------------------------------------------------
-- chat_turn_feedback : explicit user signal per turn
-- ---------------------------------------------------------------------------

CREATE TABLE chat_turn_feedback (
    id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id    UUID        NOT NULL,
    chat_turn_id  UUID        NOT NULL,
    user_id       UUID        NOT NULL,
    signal        SMALLINT    NOT NULL CHECK (signal IN (-1, 1)),
    reason        TEXT,                                  -- free-text user comment
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),

    -- A user can change their mind: UPDATE the row, don't INSERT twice.
    UNIQUE (chat_turn_id, user_id),

    CONSTRAINT ctf_turn_same_company_fk
        FOREIGN KEY (company_id, chat_turn_id)
        REFERENCES chat_turns(company_id, id) ON DELETE CASCADE,

    CONSTRAINT ctf_user_same_company_fk
        FOREIGN KEY (company_id, user_id)
        REFERENCES users(company_id, id) ON DELETE CASCADE
);

-- "All feedback for turn T" (admin review).
CREATE INDEX chat_turn_feedback_turn_idx
    ON chat_turn_feedback (company_id, chat_turn_id);

-- "All feedback from user U" (user profile / abuse review).
CREATE INDEX chat_turn_feedback_user_idx
    ON chat_turn_feedback (company_id, user_id);

-- "Recent negative signal across the company" (quality dashboard).
CREATE INDEX chat_turn_feedback_negative_idx
    ON chat_turn_feedback (company_id, created_at DESC)
    WHERE signal = -1;

CREATE TRIGGER chat_turn_feedback_updated_at
    BEFORE UPDATE ON chat_turn_feedback
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- ---------------------------------------------------------------------------
-- chunk_quality_signals : aggregated counters per chunk
--
-- Maintained by an application/worker job from chat_turn_feedback joined with
-- chat_turns.rag_sources. NOT a trigger (see header comment).
--
-- The table is composite-FK'd to document_chunks so a chunk hard-delete
-- cascades and a row is never orphaned. company_id is part of the PK so
-- the same chunk_id across tenants (theoretically impossible, but defence
-- in depth) cannot collide.
-- ---------------------------------------------------------------------------

CREATE TABLE chunk_quality_signals (
    company_id      UUID        NOT NULL,
    chunk_id        UUID        NOT NULL,
    positive_count  INT         NOT NULL DEFAULT 0 CHECK (positive_count >= 0),
    negative_count  INT         NOT NULL DEFAULT 0 CHECK (negative_count >= 0),
    last_signal_at  TIMESTAMPTZ,
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),

    PRIMARY KEY (company_id, chunk_id),

    CONSTRAINT cqs_chunk_same_company_fk
        FOREIGN KEY (company_id, chunk_id)
        REFERENCES document_chunks(company_id, id) ON DELETE CASCADE
);

-- Quality dashboard: "lowest-scoring chunks for company X".
CREATE INDEX chunk_quality_signals_negative_idx
    ON chunk_quality_signals (company_id, negative_count DESC)
    WHERE negative_count > 0;

CREATE TRIGGER chunk_quality_signals_updated_at
    BEFORE UPDATE ON chunk_quality_signals
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
