#include "wikore/adapters/postgres/unit_of_work.hpp"
#include <spdlog/spdlog.h>

namespace wikore::postgres {

drogon::Task<UnitOfWork> UnitOfWork::begin(drogon::orm::DbClientPtr db) {
    auto tx = co_await db->newTransactionCoro();
    co_return UnitOfWork{std::move(tx)};
}

UnitOfWork::~UnitOfWork() {
    if (!committed_ && tx_) {
        spdlog::warn("[uow] destroyed without commit - rolling back");
        tx_->rollback();
    }
}

} // namespace wikore::postgres
