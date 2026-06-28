-- ---------------------------------------------------------------------------
-- V030  ingest_claim_token + lifecycle gating + retry-count integrity
--
-- Three coupled fixes for race conditions in the ingest worker pool that
-- the V029 schema did not yet guard against:
--
-- 1. ingest_claim_token
--    Without a per-claim token, the worker pool has a window where
--    worker A claims a row, the polling fallback resets it to 'pending'
--    after stuck_threshold, worker B claims and completes it as 'done',
--    and then worker A's mark_ingest_done / set_ingest_status('error')
--    unconditionally overwrites B's terminal state with A's stale
--    chunk_count / error_msg. The token lets every mutation predicate
--    on "are we still the row's current claimant?" -- a mismatch
--    returns zero rows and the use case reports OwnershipLost rather
--    than corrupting state.
--
-- 2. lifecycle_status gating
--    claim_for_processing previously only checked ingest_status='pending'.
--    A pending row whose lifecycle_status was set to 'archived' (V010
--    treats archived as terminal-no-retrieval) could still be claimed,
--    chunked, and outboxed -- wasting work and producing chunks that the
--    retrieval layer would never expose. claim now requires
--    lifecycle_status IN ('draft','deprecated').
--
-- 3. ingest_retry_count >= 0
--    PollingFallback's post-commit LPUSH-rollback decrements
--    ingest_retry_count on terminal LPUSH failure. Without a CHECK
--    constraint and with concurrent sweeps, this could in principle
--    go negative. Add the CHECK as a safety net.
-- ---------------------------------------------------------------------------

BEGIN;

-- 1. Per-claim ownership token. NULL when the row is not currently
--    claimed by a worker. Set by claim_for_processing CAS. Cleared by
--    PollingFallback when resetting 'processing' to 'pending' (so the
--    next worker's CAS starts fresh) and by terminal transitions
--    (mark_ingest_done / set_ingest_status('error')).
ALTER TABLE document_versions
    ADD COLUMN IF NOT EXISTS ingest_claim_token UUID;

COMMENT ON COLUMN document_versions.ingest_claim_token IS
    'Per-claim ownership token (UUID) set by IngestDocumentVersionUseCase '
    'via the CAS pending->processing flip. Required by mark_ingest_done '
    'and set_ingest_status(error) so a worker whose claim was reset by '
    'the polling fallback (sweep #2) cannot overwrite a newer worker''s '
    'terminal state. NULL when the row is not currently claimed.';

CREATE INDEX IF NOT EXISTS document_versions_ingest_claim_token_idx
    ON document_versions (ingest_claim_token)
    WHERE ingest_claim_token IS NOT NULL;


-- 2. retry-count integrity. Cheap CHECK; no migration of existing data
--    needed because V029's column defaults to 0 and we never decrement
--    below 0 in any code path (GREATEST(... - 1, 0) guards the
--    PollingFallback rollback).
ALTER TABLE document_versions
    DROP CONSTRAINT IF EXISTS document_versions_ingest_retry_count_chk;
ALTER TABLE document_versions
    ADD CONSTRAINT document_versions_ingest_retry_count_chk
    CHECK (ingest_retry_count >= 0);

COMMIT;
