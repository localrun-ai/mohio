-- V020: Retention policy hooks for documents and chat_turns
--
-- Adds the minimum schema needed for "retain X for N days/years" policies
-- without committing to a sweep implementation. Three additions:
--
--   companies.default_retention_days INT NULL
--     Per-tenant default for newly-created records. NULL means no policy
--     (retain forever / caller-controlled).
--
--   documents.retention_until TIMESTAMPTZ NULL
--     Override: if set, this document is eligible for erasure after this
--     timestamp. NULL means "use the tenant default" - application reads
--     companies.default_retention_days and computes effective deadline
--     from documents.created_at.
--
--   chat_turns.retention_until TIMESTAMPTZ NULL
--     Same semantics, for chat turns. SOX-style "retain audit chats 7y"
--     and GDPR-style "delete user chats after 90d" pull in opposite
--     directions; per-row overrides let policy diverge from the default
--     when needed.
--
-- The actual sweep job (scheduler) is out of scope here. The schema
-- exists so policy can be set on day one; the sweep can be wired in
-- iteration 4 or later without losing rows that should already have
-- been eligible for deletion.
--
-- Why now: regulated-industry customers (legal, healthcare, gov) ask
-- about retention on day one. Without a column, the answer is "we'll
-- need to do a schema change first" - which loses sales cycles and
-- forces a backfill across every existing row when finally added.

ALTER TABLE companies
    ADD COLUMN default_retention_days INT
    CHECK (default_retention_days IS NULL OR default_retention_days > 0);

ALTER TABLE documents
    ADD COLUMN retention_until TIMESTAMPTZ;

ALTER TABLE chat_turns
    ADD COLUMN retention_until TIMESTAMPTZ;

-- Sweep hot path: find docs whose retention has elapsed.
CREATE INDEX documents_retention_due_idx
    ON documents (retention_until)
    WHERE retention_until IS NOT NULL;

-- Sweep hot path: find chat turns whose retention has elapsed.
CREATE INDEX chat_turns_retention_due_idx
    ON chat_turns (retention_until)
    WHERE retention_until IS NOT NULL;
