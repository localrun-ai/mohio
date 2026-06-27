#include "wikore/application/ingest_document_version.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <format>

namespace wikore::application {

drogon::Task<Result<void>>
IngestDocumentVersionUseCase::execute(RequestContext ctx,
                                       IngestDocumentVersionCmd cmd)
{
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("deadline exceeded"));

    // Sanity: the use case's job is to ingest a version of a document that
    // belongs to the same tenant the caller is acting as.
    if (ctx.tenant.company_id != cmd.company_id) {
        co_return std::unexpected(Error::forbidden(std::format(
            "ingest: cmd.company_id={} does not match ctx.tenant.company_id={}",
            cmd.company_id, ctx.tenant.company_id)));
    }

    spdlog::info("[ingest] start company={} doc={} version={} trace={}",
                 cmd.company_id, cmd.document_id, cmd.document_version_id,
                 ctx.span.trace_id);

    // -----------------------------------------------------------------------
    // 1. Mark 'processing' outside any UoW.
    //    If the rest of the pipeline fails, the row remains in 'processing'
    //    long enough for the scheduler's polling fallback to find it (per
    //    development_plan.md iter-1 "stuck ingest re-enqueue").
    // -----------------------------------------------------------------------
    if (auto r = co_await repo_->set_ingest_status(
            cmd.company_id, cmd.document_version_id,
            ingest::IngestStatus::processing, db_);
        !r)
    {
        co_return std::unexpected(r.error());
    }

    // Helper: mark 'error' then propagate the original error.
    auto fail = [&](Error e) -> drogon::Task<Result<void>> {
        co_await repo_->set_ingest_status(
            cmd.company_id, cmd.document_version_id,
            ingest::IngestStatus::error, db_);
        co_return std::unexpected(std::move(e));
    };

    // -----------------------------------------------------------------------
    // 2. Read file
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
    // 3. Parse + chunk (pure CPU; no DB connection held)
    // -----------------------------------------------------------------------
    auto doc = parser_->parse(content, cmd.file_path, "text/plain");
    spdlog::debug("[ingest] parsed {} sections, {} chars",
                  doc.sections.size(), doc.full_text.size());

    // -----------------------------------------------------------------------
    // 4. Resolve access scopes for the chunk denormalization.
    //    The PG read here uses the bare client (not a UoW) and the
    //    connection is released before the UoW begins.
    // -----------------------------------------------------------------------
    auto scopes_result = co_await repo_->fetch_access_scopes(
        cmd.company_id, cmd.document_id);
    if (!scopes_result)
        co_return co_await fail(scopes_result.error());
    const auto& access_scope_ids = *scopes_result;

    // -----------------------------------------------------------------------
    // 5. Atomic write: sections + chunks + audit + outbox + mark done.
    //    Per the UoW contract (PR #1), all of these commit together or
    //    none do. The Qdrant side-effect is a row in outbox_events, not
    //    an HTTP call -- a worker drains the outbox and calls Qdrant.
    // -----------------------------------------------------------------------
    auto uow = co_await postgres::UnitOfWork::begin(db_);

    if (auto r = co_await repo_->write_sections(
            doc, cmd.document_version_id, cmd.company_id, uow);
        !r)
    {
        uow.rollback();
        co_return co_await fail(r.error());
    }

    auto chunks = chunker_.chunk(doc, cmd.document_version_id, cmd.company_id);
    spdlog::debug("[ingest] produced {} chunks", chunks.size());

    if (auto r = co_await repo_->write_chunks(
            chunks, cmd.company_id, access_scope_ids, uow);
        !r)
    {
        uow.rollback();
        co_return co_await fail(r.error());
    }

    // Audit row: same UoW.
    const auto actor_type =
        ctx.principal.is_service_account ? "service" : "user";
    std::optional<Error> audit_err;
    try {
        co_await uow.exec(
            R"(INSERT INTO audit_log
                   (company_id, user_id, action, detail)
               VALUES ($1::uuid, $2::uuid, 'doc.ingest.chunks_written',
                       jsonb_build_object(
                           'actor_type',     $3::text,
                           'document_id',    $4::text,
                           'version_id',     $5::text,
                           'chunk_count',    $6::int,
                           'section_count',  $7::int,
                           'embed_model_id', $8::text)))",
            ctx.tenant.company_id,
            ctx.principal.user_id,
            actor_type,
            cmd.document_id,
            cmd.document_version_id,
            static_cast<int>(chunks.size()),
            static_cast<int>(doc.sections.size()),
            cmd.embed_model_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        audit_err = postgres::map_db_exception(ex);
    }
    if (audit_err) {
        uow.rollback();
        co_return co_await fail(std::move(*audit_err));
    }

    // Outbox row: same UoW. The worker drains this and performs the actual
    // embed + Qdrant upsert. Idempotency key includes trace_id so client
    // retries are bounded (re-ingest of the same version under the same
    // trace_id is a no-op via ON CONFLICT DO NOTHING).
    const auto idempotency_key = std::format(
        "qdrant_upsert:{}:{}:{}",
        cmd.document_version_id, cmd.embed_model_id, ctx.span.trace_id);
    std::optional<Error> outbox_err;
    try {
        co_await uow.exec(
            R"(INSERT INTO outbox_events
                   (company_id, aggregate_id, job_type, payload, idempotency_key)
               VALUES ($1::uuid, $2::uuid, 'qdrant_upsert_chunk_payload',
                       jsonb_build_object(
                           'document_id',       $3::text,
                           'document_version_id', $4::text,
                           'embed_model_id',    $5::text,
                           'chunk_count',       $6::int),
                       $7)
               ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING)",
            ctx.tenant.company_id,
            cmd.document_version_id,   // aggregate_id
            cmd.document_id,
            cmd.document_version_id,
            cmd.embed_model_id,
            static_cast<int>(chunks.size()),
            idempotency_key);
    } catch (const drogon::orm::DrogonDbException& ex) {
        outbox_err = postgres::map_db_exception(ex);
    }
    if (outbox_err) {
        uow.rollback();
        co_return co_await fail(std::move(*outbox_err));
    }

    // Mark done inside the UoW. document_versions_done_state_chk requires
    // completed_at AND chunk_count to be non-NULL; mark_ingest_done sets
    // both atomically.
    if (auto r = co_await repo_->mark_ingest_done(
            cmd.company_id, cmd.document_version_id,
            static_cast<int>(chunks.size()), uow);
        !r)
    {
        uow.rollback();
        co_return co_await fail(r.error());
    }

    co_await uow.commit();

    spdlog::info("[ingest] done company={} version={} chunks={}",
                 cmd.company_id, cmd.document_version_id, chunks.size());
    co_return Result<void>{};
}

} // namespace wikore::application
