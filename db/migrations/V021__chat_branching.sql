-- V021: chat_turns.parent_turn_id for branched conversations
--
-- Modern chat UIs ("edit message, regenerate from here") produce a tree
-- of turns rather than a flat list. Without parent_turn_id, branches
-- have to be inferred from timestamps and request bodies - and the
-- correct tree is unrecoverable for turns that already happened
-- without the column.
--
-- Why now: cheap to add empty, painful to backfill. Nullable so existing
-- turns (which have no parent) are valid as-is and new top-level turns
-- can leave it NULL.
--
-- FK is a simple self-FK rather than a composite (company_id, parent_turn_id).
-- chat_turns already enforces same-company via its session FK; same-session
-- between a turn and its parent is an application invariant (ContextBuilder
-- only branches within the same session). Following the V005 design choice
-- to keep some chat invariants application-enforced.
--
-- ON DELETE SET NULL: if a parent is hard-deleted, child turns become
-- orphaned but survive. Soft-delete via retention sweep is the normal path.

ALTER TABLE chat_turns
    ADD COLUMN parent_turn_id UUID;

ALTER TABLE chat_turns
    ADD CONSTRAINT chat_turns_parent_fk
    FOREIGN KEY (parent_turn_id)
    REFERENCES chat_turns(id) ON DELETE SET NULL;

-- Forward traversal: "list children of turn T" - branch view in UI.
CREATE INDEX chat_turns_parent_idx
    ON chat_turns (parent_turn_id)
    WHERE parent_turn_id IS NOT NULL;
