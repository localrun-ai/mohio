-- V015: Transactional outbox for Qdrant and Redis side-effects
--
-- Every state change that requires a Qdrant or Redis follow-up is recorded
-- here in the same Postgres transaction as the state change itself.
-- This eliminates the crash gap between "row committed" and "job enqueued":
-- if the API process dies after committing but before pushing to Redis,
-- the outbox row survives and wikore-scheduler picks it up on the next poll.
--
-- Consumer pattern: SELECT ... FOR UPDATE SKIP LOCKED (scheduler).
-- Idempotency: UNIQUE (company_id, job_type, idempotency_key) prevents
-- duplicate side effects when the same outbox event is claimed twice
-- (e.g. after a network timeout where the worker is unsure if it committed).
--
-- job_schema_version: bumped when the payload structure changes. Workers
-- must reject and re-queue events with a schema version they don't understand,
-- allowing rolling deploys without corrupting state.
--
-- claimed_by stores a worker instance token (hostname:pid) so stuck jobs
-- can be detected: claimed_at IS NOT NULL AND completed_at IS NULL AND
-- claimed_at < now() - interval 'N minutes' implies the claiming worker died.
-- wikore-scheduler re-enqueues such jobs.

CREATE TABLE outbox_events (
    id                 UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    company_id         UUID        NOT NULL REFERENCES companies(id) ON DELETE CASCADE,
    aggregate_id       UUID,                       -- doc_id, chunk_id, user_id, etc.
    job_type           TEXT        NOT NULL,        -- 'qdrant_upsert_chunk_payload', etc.
    job_schema_version SMALLINT    NOT NULL DEFAULT 1,
    payload            JSONB       NOT NULL,
    idempotency_key    TEXT        NOT NULL,
    claimed_at         TIMESTAMPTZ,
    claimed_by         TEXT,                       -- 'hostname:pid' of claiming worker
    completed_at       TIMESTAMPTZ,
    attempt_count      INT         NOT NULL DEFAULT 0,
    last_error         TEXT,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),

    UNIQUE (company_id, job_type, idempotency_key)
);

-- Scheduler poll: unclaimed and not yet complete, ordered by creation time.
CREATE INDEX outbox_events_pending_idx
    ON outbox_events (company_id, created_at)
    WHERE completed_at IS NULL AND claimed_at IS NULL;

-- Stuck-job detection: claimed but not completed, ordered by claim time.
CREATE INDEX outbox_events_stuck_idx
    ON outbox_events (claimed_at)
    WHERE completed_at IS NULL AND claimed_at IS NOT NULL;

-- Audit / replay: all events for a given aggregate.
CREATE INDEX outbox_events_aggregate_idx
    ON outbox_events (company_id, aggregate_id)
    WHERE aggregate_id IS NOT NULL;
