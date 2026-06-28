-- ---------------------------------------------------------------------------
-- V029  outbox backoff + ingest crash-resume payload
--
-- Two related reliability fixes for Iteration 1:
--
-- 1. outbox_events.next_attempt_at
--    Without persisted backoff, every transient downstream failure
--    (Qdrant blip, llama-server timeout, network partition) clears the
--    claim immediately and the run loop retries after only poll_interval
--    (default 500 ms). With max_attempts=5, an outage lasting ~2.5 s burns
--    the entire retry budget and the event becomes permanently
--    unclaimable. The fix: each failure sets next_attempt_at to
--    now() + exp_backoff(attempt_count), and claim_batch skips events
--    whose retry window hasn't arrived. Cap is bounded (5 minutes) so a
--    long outage never strands events indefinitely; the scheduler picks
--    them up at most once per cap interval after recovery.
--
-- 2. document_versions.ingest_job_payload + ingest_retry_count
--    Iteration 1's crash-recovery contract requires "kill the worker
--    mid-ingest, restart, the version reaches 'done'". With the previous
--    behaviour, a worker that crashed after RPOPing the Redis job and
--    flipping ingest_status='processing' lost the job permanently --
--    the stuck sweep could only promote the row to 'error'. The fix:
--    persist the IngestJob payload on the version row when the worker
--    claims it (worker.cpp's dispatch step), so the polling sweep can
--    re-enqueue the payload to Redis and reset the row to 'pending'.
--    A retry counter caps the requeue budget so a poison message
--    eventually terminates at 'error' rather than looping forever.
-- ---------------------------------------------------------------------------

BEGIN;

-- ---------------------------------------------------------------------------
-- 1. outbox_events.next_attempt_at
-- ---------------------------------------------------------------------------

ALTER TABLE outbox_events
    ADD COLUMN IF NOT EXISTS next_attempt_at TIMESTAMPTZ NOT NULL DEFAULT now();

COMMENT ON COLUMN outbox_events.next_attempt_at IS
    'Earliest timestamp at which this event may be re-claimed. '
    'Updated by mark_failed to now() + exponential backoff so transient '
    'downstream outages do not exhaust attempt_count in a few seconds.';

-- The pending index must include next_attempt_at because claim_batch now
-- filters on it. Drop the old partial index, recreate.
DROP INDEX IF EXISTS outbox_events_pending_idx;

CREATE INDEX outbox_events_pending_idx
    ON outbox_events (next_attempt_at)
    WHERE completed_at IS NULL AND claimed_at IS NULL;

COMMENT ON INDEX outbox_events_pending_idx IS
    'Scheduler poll: unclaimed events whose retry window has arrived, '
    'ordered by next_attempt_at so the oldest eligible event claims first.';


-- ---------------------------------------------------------------------------
-- 2. document_versions.ingest_job_payload + ingest_retry_count
--
-- ingest_job_payload is the IngestJob JSON the worker LMOVEs from Redis
-- BEFORE invoking the use case. The sweep uses it to re-enqueue the
-- payload when a worker dies mid-job. Nullable because rows that have
-- never been claimed (still pending) won't have it set, and rows that
-- already reached 'done' don't need it (it's cleared on completion).
-- ---------------------------------------------------------------------------

ALTER TABLE document_versions
    ADD COLUMN IF NOT EXISTS ingest_job_payload JSONB,
    ADD COLUMN IF NOT EXISTS ingest_retry_count INT NOT NULL DEFAULT 0;

COMMENT ON COLUMN document_versions.ingest_job_payload IS
    'IngestJob payload (file_path, embed_model_id, ...) persisted by the '
    'ingest worker when it claims the job, so PollingFallback can re-enqueue '
    'after a worker crash. Cleared when ingest_status reaches done/error.';

COMMENT ON COLUMN document_versions.ingest_retry_count IS
    'Number of times PollingFallback has re-enqueued this version after a '
    'mid-job crash. Caps the requeue budget so a poison message eventually '
    'terminates at ingest_status=error rather than looping forever.';

COMMIT;
