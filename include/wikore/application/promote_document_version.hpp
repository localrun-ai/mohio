#pragma once
#include "wikore/domain/types.hpp"
#include <drogon/orm/DbClient.h>

namespace wikore::application {

// ---------------------------------------------------------------------------
// PromoteDocumentVersion use case
//
// Promotes a document_version to lifecycle_status='active' by calling the
// promote_document_version() SQL function (V010), then writes an audit_log
// row and an outbox_event row in the same Postgres transaction.
//
// This is the Iteration 0 exemplar use case: it exercises UnitOfWork,
// pg_error_mapper, audit writing, and outbox writing atomically.
// ---------------------------------------------------------------------------

struct PromoteDocumentVersionCmd {
    Uuid document_id;
    Uuid version_id;
};

class PromoteDocumentVersionUseCase {
public:
    explicit PromoteDocumentVersionUseCase(drogon::orm::DbClientPtr db)
        : db_(std::move(db)) {}

    drogon::Task<Result<void>>
    execute(const RequestContext& ctx, PromoteDocumentVersionCmd cmd);

private:
    drogon::orm::DbClientPtr db_;
};

} // namespace wikore::application
