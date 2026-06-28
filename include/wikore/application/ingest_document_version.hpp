#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/ingest/types.hpp"
#include "wikore/ingest/chunker.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/ingest/parser.hpp"
#include "wikore/rag/embedder.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>

namespace wikore::application {

// ---------------------------------------------------------------------------
// IngestDocumentVersionCmd: drives the ingest pipeline for one document version.
//
// Fields must match the information available on an IngestJob queue message.
// ---------------------------------------------------------------------------

struct IngestDocumentVersionCmd {
    std::string company_id;
    std::string document_id;
    std::string document_version_id;
    std::string file_path;       // absolute path on shared storage
    std::string embed_model_id;  // e.g. "bge-m3"

    // Optional: the original IngestJob JSON payload as it appeared on
    // the Redis queue. When set, execute() uses an atomic CAS UPDATE
    // (pending -> processing AND persist payload in one statement) so
    // duplicate delivery from the polling fallback's orphan reaper is
    // safely absorbed without resurrecting terminal rows. When empty,
    // execute() falls back to the unconditional set_ingest_status flip
    // (used by tests that call the use case directly).
    std::string ingest_job_payload;
};

// ---------------------------------------------------------------------------
// IngestDocumentVersionUseCase
//
// Orchestrates the write path of ingest:
//   1. mark version 'processing' (outside the UoW so the transition is
//      visible to operators even if step 3 fails)
//   2. read file -> parse -> chunk
//   3. inside one UoW:
//      - upsert document_sections
//      - upsert document_chunks (with access_scope_ids denormalized)
//      - INSERT audit_log row ('doc_ingest_chunks_written')
//      - INSERT outbox_events row (job_type='qdrant_upsert_chunk_payload')
//      - mark_ingest_done() (sets ingest_status='done', completed_at,
//        chunk_count -- satisfies document_versions_done_state_chk)
//      - co_await uow.commit()
//
// The actual Qdrant upsert is performed by the worker that drains the
// outbox -- NOT in this use case. This preserves the V015 outbox crash-
// safety contract: every Qdrant side-effect is durable in PG before any
// HTTP call happens, and re-running ingest after a crash is safe because
// the outbox event is idempotent by (company_id, job_type, idempotency_key).
//
// On any partial failure: sets ingest_status='error' (outside the UoW)
// and returns the error. The caller (worker loop) decides whether to retry.
// ---------------------------------------------------------------------------

// IngestDispatchOutcome: result type for IngestDocumentVersionUseCase::execute.
//
// Distinguishes:
//   * Processed         - we won the CAS, ran the pipeline, mark_done succeeded
//   * TerminalError     - we won the CAS, pipeline failed, row is now 'error'
//                         (terminal: caller should LREM, clear payload, NOT retry)
//   * DuplicateSkipped  - we lost the CAS at claim time (row was not pending);
//                         caller LREMs proc entry without touching the row
//   * OwnershipLost     - we won the claim CAS but lost the row before
//                         mark_done/mark_error (polling fallback reset us;
//                         another worker now owns the row). Caller LREMs proc
//                         entry; the new owner handles the row's lifecycle.
//
// The Err return path is reserved for INFRA failures that happen BEFORE the
// CAS can win (e.g. DB connectivity, deadline exceeded). The worker
// distinguishes Err from TerminalError when deciding whether to transfer
// the proc entry back to the source queue.
enum class IngestDispatchOutcome {
    Processed,
    TerminalError,
    DuplicateSkipped,
    OwnershipLost,
};

class IngestDocumentVersionUseCase {
public:
    IngestDocumentVersionUseCase(
        drogon::orm::DbClientPtr                  db,
        std::shared_ptr<ingest::DocumentRepoPort> repo,
        std::shared_ptr<ingest::ParserPort>       parser,
        ingest::Chunker                           chunker)
        : db_(std::move(db))
        , repo_(std::move(repo))
        , parser_(std::move(parser))
        , chunker_(std::move(chunker))
    {}

    // By-value RequestContext + cmd: Drogon coroutines suspend before the
    // first line (initial_suspend = suspend_always); references to caller
    // temporaries dangle by the time the body runs.
    //
    // Returns:
    //   Ok(Processed)        - claim won; pipeline succeeded or fail'd
    //                          internally; row is now 'done' or 'error'.
    //   Ok(DuplicateSkipped) - claim lost; row was not 'pending'; this
    //                          worker did NOT modify document_versions or
    //                          any outbox state. Caller should LREM the
    //                          proc entry without touching the row.
    //   Err(e)               - real failure (DB connectivity, etc.);
    //                          caller handles per its policy.
    drogon::Task<Result<IngestDispatchOutcome>>
    execute(RequestContext ctx, IngestDocumentVersionCmd cmd);

private:
    drogon::orm::DbClientPtr                  db_;
    std::shared_ptr<ingest::DocumentRepoPort> repo_;
    std::shared_ptr<ingest::ParserPort>       parser_;
    ingest::Chunker                           chunker_;
};

} // namespace wikore::application
