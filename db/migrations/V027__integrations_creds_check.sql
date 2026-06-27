-- V027: integrations credentials/key_id presence CHECK
--
-- V006 declares integrations.credentials (encrypted blob) and
-- integrations.credentials_key_id (which key version encrypted it) as
-- independent nullable columns. That allows representing an orphan
-- ciphertext: credentials non-NULL but credentials_key_id NULL means we
-- have a blob with no way to know which key would decrypt it.
--
-- Add a CHECK so the two fields move together. Application's responsibility
-- to populate both (or neither, for "integration exists but no credentials
-- attached yet"); the constraint catches the partial-write bug at the DB.
--
-- Why now: cheap, additive. Without it, a future bug in the integration
-- create/rotate path could silently land orphan ciphertext that nobody
-- notices until decryption fails at runtime.

ALTER TABLE integrations
    ADD CONSTRAINT integrations_credentials_key_id_consistent_chk
    CHECK ((credentials IS NULL) = (credentials_key_id IS NULL));
