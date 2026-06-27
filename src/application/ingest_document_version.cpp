#include "wikore/application/ingest_document_version.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/rag/types.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <format>

namespace wikore::application {

drogon::Task<Result<void>>
IngestDocumentVersionUseCase::execute(const IngestDocumentVersionCmd& cmd)
{
    spdlog::info("[ingest] start company={} version={}",
                 cmd.company_id, cmd.document_version_id);

    // Mark version as running immediately so the UI shows progress.
    if (auto r = co_await repo_->set_ingest_status(
            cmd.company_id, cmd.document_version_id,
            ingest::IngestStatus::running, db_); !r)
    {
        co_return std::unexpected(r.error());
    }

    // Helper: mark as failed then propagate the error
    auto fail = [&](Error e) -> drogon::Task<Result<void>> {
        co_await repo_->set_ingest_status(
            cmd.company_id, cmd.document_version_id,
            ingest::IngestStatus::failed, db_);
        co_return std::unexpected(std::move(e));
    };

    // -----------------------------------------------------------------------
    // 1. Read file from shared storage
    // -----------------------------------------------------------------------
    std::string content;
    {
        std::ifstream f(cmd.file_path, std::ios::binary);
        if (!f) {
            co_return co_await fail(
                Error::invalid_input(std::format("cannot open {}", cmd.file_path)));
        }
        std::ostringstream buf;
        buf << f.rdbuf();
        content = buf.str();
    }

    // -----------------------------------------------------------------------
    // 2. Parse
    // -----------------------------------------------------------------------
    auto doc = parser_->parse(content, cmd.file_path, "text/plain");
    spdlog::debug("[ingest] parsed {} sections, {} chars",
                  doc.sections.size(), doc.full_text.size());

    // -----------------------------------------------------------------------
    // 3. Fetch access scopes for the Qdrant payload
    // -----------------------------------------------------------------------
    auto scopes_result = co_await repo_->fetch_access_scopes(
        cmd.company_id, cmd.document_id);
    if (!scopes_result)
        co_return co_await fail(scopes_result.error());

    // -----------------------------------------------------------------------
    // 4. Write sections + chunks inside a transaction
    // -----------------------------------------------------------------------
    auto uow = co_await postgres::UnitOfWork::begin(db_);

    if (auto r = co_await repo_->write_sections(
            doc, cmd.document_version_id, cmd.company_id, uow); !r)
    {
        uow.rollback();
        co_return co_await fail(r.error());
    }

    auto chunks = chunker_.chunk(doc, cmd.document_version_id, cmd.company_id);
    spdlog::debug("[ingest] produced {} chunks", chunks.size());

    if (auto r = co_await repo_->write_chunks(chunks, cmd.company_id, uow); !r) {
        uow.rollback();
        co_return co_await fail(r.error());
    }

    co_await uow.commit();

    // -----------------------------------------------------------------------
    // 5. Embed chunks in batches of 32
    // -----------------------------------------------------------------------
    constexpr int kBatchSize = 32;
    std::vector<rag::UpsertPoint> points;
    points.reserve(chunks.size());

    for (int i = 0; i < static_cast<int>(chunks.size()); i += kBatchSize) {
        int end = std::min(i + kBatchSize, static_cast<int>(chunks.size()));
        std::vector<std::string> texts;
        texts.reserve(end - i);
        for (int j = i; j < end; ++j)
            texts.push_back(chunks[j].text);

        auto embed_result = co_await embedder_->embed_batch(std::move(texts));
        if (!embed_result)
            co_return co_await fail(embed_result.error());

        const auto& embeddings = *embed_result;
        for (int j = 0; j < static_cast<int>(embeddings.size()); ++j) {
            const auto& chunk = chunks[i + j];
            const auto chunk_id = chunk.db_id.value_or(
                chunk.document_version_id + ":" + std::to_string(chunk.chunk_index));

            rag::ChunkPayload payload{
                .company_id          = cmd.company_id,
                .document_id         = cmd.document_id,
                .document_version_id = cmd.document_version_id,
                .chunk_id            = chunk_id,
                .chunk_index         = chunk.chunk_index,
                .access_scope_ids    = *scopes_result,
                .lifecycle_status    = "draft",
                .section_id          = chunk.section_id,
                .section_heading     = chunk.section_heading,
            };

            const auto point_id = rag::uuid_v5(
                chunk_id + ":" + cmd.embed_model_id);

            points.push_back({point_id, embeddings[j], std::move(payload)});
        }
    }

    // -----------------------------------------------------------------------
    // 6. Upsert to Qdrant
    // -----------------------------------------------------------------------
    if (auto r = co_await vector_store_->upsert(points); !r)
        co_return co_await fail(r.error());

    // -----------------------------------------------------------------------
    // 7. Mark done
    // -----------------------------------------------------------------------
    if (auto r = co_await repo_->set_ingest_status(
            cmd.company_id, cmd.document_version_id,
            ingest::IngestStatus::done, db_); !r)
    {
        co_return std::unexpected(r.error());
    }

    spdlog::info("[ingest] done company={} version={} chunks={}",
                 cmd.company_id, cmd.document_version_id, chunks.size());
    co_return Result<void>{};
}

} // namespace wikore::application
