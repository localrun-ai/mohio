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
    drogon::Task<Result<void>>
    execute(RequestContext ctx, IngestDocumentVersionCmd cmd);

private:
    drogon::orm::DbClientPtr                  db_;
    std::shared_ptr<ingest::DocumentRepoPort> repo_;
    std::shared_ptr<ingest::ParserPort>       parser_;
    ingest::Chunker                           chunker_;
};

} // namespace wikore::application
