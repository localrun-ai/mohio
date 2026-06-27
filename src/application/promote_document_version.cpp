#include "wikore/application/promote_document_version.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <spdlog/spdlog.h>
#include <format>

namespace wikore::application {

drogon::Task<Result<void>>
PromoteDocumentVersionUseCase::execute(
    const RequestContext& ctx,
    PromoteDocumentVersionCmd cmd)
{
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("deadline exceeded"));

    auto uow = co_await postgres::UnitOfWork::begin(db_);

    // Step 1: call the promote_document_version() SQL function (V010).
    // This atomically deprecates the current active version and activates
    // the requested version. Throws if: version not found, ingest not done,
    // or version is already archived (archived = terminal).
    try {
        co_await uow.exec(
            "SELECT promote_document_version($1, $2, $3)",
            ctx.tenant.company_id,
            cmd.document_id,
            cmd.version_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        uow.rollback();
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // Step 2: write audit log row in the same transaction.
    try {
        co_await uow.exec(
            R"(INSERT INTO audit_log
                   (company_id, actor_id, actor_type, action, entity_type, entity_id, detail)
               VALUES ($1, $2, 'user', 'document.version.promoted', 'document_version', $3,
                       jsonb_build_object('document_id', $4::text)))",
            ctx.tenant.company_id,
            ctx.principal.user_id,
            cmd.version_id,
            cmd.document_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        uow.rollback();
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // Step 3: write outbox event for Qdrant payload resync in the same transaction.
    const auto idempotency_key = std::format("promote:{}:{}", cmd.document_id, cmd.version_id);
    try {
        co_await uow.exec(
            R"(INSERT INTO outbox_events
                   (company_id, aggregate_id, job_type, payload, idempotency_key)
               VALUES ($1, $2, 'qdrant_resync_version_lifecycle',
                       jsonb_build_object(
                           'document_id',  $3::text,
                           'version_id',   $2::text,
                           'new_status',   'active'
                       ),
                       $4)
               ON CONFLICT (company_id, job_type, idempotency_key) DO NOTHING)",
            ctx.tenant.company_id,
            cmd.version_id,
            cmd.document_id,
            idempotency_key);
    } catch (const drogon::orm::DrogonDbException& ex) {
        uow.rollback();
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // Commit: release the transaction shared_ptr -> Drogon auto-commits.
    // State change + audit + outbox are atomic in the same Postgres transaction.
    uow.commit();

    spdlog::info("[promote_version] company={} doc={} version={} promoted by user={}",
                 ctx.tenant.company_id, cmd.document_id, cmd.version_id,
                 ctx.principal.user_id);
    co_return Result<void>{};
}

} // namespace wikore::application
