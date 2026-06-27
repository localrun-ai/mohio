#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/ingest/types.hpp"
#include "wikore/ingest/chunker.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/ingest/parser.hpp"
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/vector_store.hpp"
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
// Orchestrates: read file -> parse -> chunk -> write PG sections/chunks ->
// embed_batch -> upsert Qdrant -> update ingest_status.
//
// On any partial failure: sets ingest_status = 'failed' and returns an error.
// The caller (worker loop) decides whether to retry or dead-letter the job.
// ---------------------------------------------------------------------------

class IngestDocumentVersionUseCase {
public:
    IngestDocumentVersionUseCase(
        drogon::orm::DbClientPtr              db,
        std::shared_ptr<ingest::DocumentRepoPort> repo,
        std::shared_ptr<ingest::ParserPort>       parser,
        ingest::Chunker                           chunker,
        std::shared_ptr<rag::EmbedderPort>        embedder,
        std::shared_ptr<rag::VectorStorePort>     vector_store)
        : db_(std::move(db))
        , repo_(std::move(repo))
        , parser_(std::move(parser))
        , chunker_(std::move(chunker))
        , embedder_(std::move(embedder))
        , vector_store_(std::move(vector_store))
    {}

    drogon::Task<Result<void>>
    execute(const IngestDocumentVersionCmd& cmd);

private:
    drogon::orm::DbClientPtr                  db_;
    std::shared_ptr<ingest::DocumentRepoPort> repo_;
    std::shared_ptr<ingest::ParserPort>       parser_;
    ingest::Chunker                           chunker_;
    std::shared_ptr<rag::EmbedderPort>        embedder_;
    std::shared_ptr<rag::VectorStorePort>     vector_store_;
};

} // namespace wikore::application
