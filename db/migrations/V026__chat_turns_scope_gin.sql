-- V026: GIN index on chat_turns.access_scope_ids
--
-- chat_turns.access_scope_ids (V005) is documented as "an immutable snapshot
-- of the org_unit IDs that were searched, forming an audit record of what
-- evidence was eligible." The natural audit query is:
--
--   "show me every chat turn that touched org X"
--     -> WHERE access_scope_ids @> ARRAY[$ou]::uuid[]
--
-- Today that's a seq scan over chat_turns. document_chunks already has the
-- equivalent index (document_chunks_scopes_idx, V003); this is the symmetric
-- index for the audit side.
--
-- Why now: cheap, additive, no behavioral change. Adding later means
-- waiting for an admin to complain about slow audit dashboards on a
-- production-sized chat_turns table.

CREATE INDEX chat_turns_access_scope_idx
    ON chat_turns USING GIN (access_scope_ids);
