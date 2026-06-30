#include "wikore/application/ingest_document_version.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <format>

namespace wikore::application {

drogon::Task<Result<IngestDispatchOutcome>>
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
    // 1. Claim the row for processing.
    //
    //    Always via claim_for_processing: a CAS that flips pending ->
    //    processing AND persists the IngestJob payload AND generates a
    //    per-claim ownership token (UUID) in one atomic statement.
    //    We pass the cmd's payload (empty allowed -- the column stores
    //    NULL for tests that bypass the worker).
    //
    //    Returns:
    //      * std::nullopt -> CAS lost (row already processing/done/error,
    //        or row is archived). Treat as DuplicateSkipped no-op.
    //      * non-empty UUID -> we own the claim. Carry the token to
    //        every terminal mutation (mark_ingest_done / set_ingest_error)
    //        so a worker whose claim is later reset by the polling
    //        fallback cannot overwrite a newer worker's terminal state.
    //
    //    Stays outside any UoW so a failure in steps 2+ leaves the row in
    //    'processing' long enough for the scheduler's polling fallback to
    //    find it (per development_plan.md iter-1 "stuck ingest re-enqueue").
    // -----------------------------------------------------------------------
    std::string claim_token;
    {
        auto claim = co_await repo_->claim_for_processing(
            cmd.company_id, cmd.document_version_id,
            cmd.ingest_job_payload, db_);
        if (!claim)
            co_return std::unexpected(claim.error());
        if (!claim->has_value()) {
            spdlog::info("[ingest] duplicate delivery for version {}; "
                         "row is not claimable (likely already processing, "
                         "done, error, or archived). Treating as no-op.",
                         cmd.document_version_id);
            co_return IngestDispatchOutcome::DuplicateSkipped;
        }
        claim_token = std::move(**claim);
    }

    // Helper: terminal-error path. Sets ingest_status='error' (gated by
    // claim_token so we never resurrect another worker's done/error
    // state) and returns either TerminalError (we owned, row is now
    // 'error') or OwnershipLost (the polling fallback reset our claim
    // while we were working).
    auto fail = [&, this](Error e)
        -> drogon::Task<Result<IngestDispatchOutcome>>
    {
        auto r = co_await repo_->set_ingest_status(
            cmd.company_id, cmd.document_version_id,
            ingest::IngestStatus::error, claim_token, e.message, db_);
        if (!r) {
            // The set_ingest_status DB call itself failed. We don't know
            // whether the row is in 'error' or not. Propagate as Err so
            // the worker leaves the proc entry in place (transferred
            // back) for recovery on the next dispatch.
            spdlog::error("[ingest] fail(): set_ingest_status('error') "
                          "failed: {}; original error was: {}",
                          r.error().message, e.message);
            co_return std::unexpected(std::move(e));
        }
        if (!*r) {
            spdlog::warn("[ingest] fail(): ownership lost for version {} "
                         "(claim token no longer matches; polling "
                         "fallback reset us). Original error was: {}",
                         cmd.document_version_id, e.message);
            co_return IngestDispatchOutcome::OwnershipLost;
        }
        co_return IngestDispatchOutcome::TerminalError;
    };

    // -----------------------------------------------------------------------
    // 2. Read file
    // -----------------------------------------------------------------------
    std::string content;
    {
        std::error_code size_error;
        const auto file_size = std::filesystem::file_size(cmd.file_path, size_error);
        if (size_error) {
            co_return co_await fail(
                Error::invalid_input(std::format("cannot stat {}", cmd.file_path)));
        }
        if (file_size > ingest::ParserPort::kMaxInputBytes) {
            co_return co_await fail(Error::invalid_input("ingest.file_too_large"));
        }

        std::ifstream f(cmd.file_path, std::ios::binary);
        if (!f) {
            co_return co_await fail(
                Error::invalid_input(std::format("cannot open {}", cmd.file_path)));
        }
        content.resize(static_cast<std::size_t>(file_size));
        f.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (f.gcount() != static_cast<std::streamsize>(content.size())) {
            co_return co_await fail(
                Error::invalid_input(std::format("cannot read {}", cmd.file_path)));
        }
        char extra = 0;
        if (f.get(extra)) {
            co_return co_await fail(
                Error::invalid_input("ingest.file_changed_during_read"));
        }
    }

    // -----------------------------------------------------------------------
    // 3. Parse + chunk (pure CPU; no DB connection held)
    // -----------------------------------------------------------------------
    auto parsed = parser_->parse(content, cmd.file_path, {});
    if (!parsed)
        co_return co_await fail(parsed.error());
    auto& doc = *parsed;
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
            chunks, cmd.company_id, cmd.document_version_id, access_scope_ids, uow);
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
    // embed + Qdrant upsert. The idempotency key is keyed on
    // (version_id, embed_model_id) ONLY -- NOT on trace_id. The job is
    // "embed this version under this model"; it does not matter how many
    // distinct HTTP requests asked for it. Including trace_id (Opus
    // finding H) defeated cross-request dedup: two genuinely distinct
    // requests for the same version produced two outbox rows and the
    // worker ran the embed/upsert twice. The duplicate work was harmless
    // (the deterministic Qdrant point id meant the second upsert was a
    // no-op) but the duplicate event still costs an embed batch and a
    // round-trip per chunk. trace_id remains in the payload so the worker
    // can stitch its logs back to the originating span.
    const auto idempotency_key = std::format(
        "qdrant_upsert:{}:{}",
        cmd.document_version_id, cmd.embed_model_id);
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
                           'chunk_count',       $6::int,
                           'trace_id',          $7::text),
                       $8)
               ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING)",
            ctx.tenant.company_id,
            cmd.document_version_id,   // aggregate_id
            cmd.document_id,
            cmd.document_version_id,
            cmd.embed_model_id,
            static_cast<int>(chunks.size()),
            ctx.span.trace_id,
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
    // both atomically. Ownership-gated by claim_token so a worker that
    // was reset by the polling fallback cannot overwrite a newer
    // worker's terminal state.
    auto done = co_await repo_->mark_ingest_done(
            cmd.company_id, cmd.document_version_id,
            static_cast<int>(chunks.size()), claim_token, uow);
    if (!done) {
        uow.rollback();
        co_return co_await fail(done.error());
    }
    if (!*done) {
        // OwnershipLost: the polling fallback reset our claim while we
        // were processing, and a newer worker now owns this row.
        // Rollback our (now-stale) UoW so we don't pollute chunks/
        // outbox with our work; the new worker will write its own.
        uow.rollback();
        spdlog::warn("[ingest] mark_ingest_done: ownership lost for "
                     "version {}; rolling back UoW. The new owner is "
                     "responsible for the row's terminal transition.",
                     cmd.document_version_id);
        co_return IngestDispatchOutcome::OwnershipLost;
    }

    // COMMIT can fail (e.g. deferred constraint, server disconnect). When it
    // does, PG rolled back -- document_versions stays 'processing' with our
    // claim_token intact. Sweep #2 will reset and requeue if no worker picks
    // it up. Return Err so the worker's transfer-back path runs.
    if (auto r = co_await uow.commit(); !r) {
        spdlog::error("[ingest] uow.commit() failed for version {}: {}; "
                      "row remains 'processing' (PG rolled back)",
                      cmd.document_version_id, r.error().message);
        co_return std::unexpected(r.error());
    }

    spdlog::info("[ingest] done company={} version={} chunks={}",
                 cmd.company_id, cmd.document_version_id, chunks.size());
    co_return IngestDispatchOutcome::Processed;
}

} // namespace wikore::application
