#include "wikore/adapters/postgres/unit_of_work.hpp"
#include <spdlog/spdlog.h>
#include <trantor/utils/AsyncFileLogger.h>

namespace wikore::postgres {

drogon::Task<UnitOfWork> UnitOfWork::begin(drogon::orm::DbClientPtr db) {
    auto tx = co_await db->newTransactionCoro();
    co_return UnitOfWork{std::move(tx)};
}

drogon::Task<void> UnitOfWork::commit() {
    // Use setCommitCallback to get a signal when PG acknowledges COMMIT.
    // Build a coroutine-friendly awaitable around the callback.
    //
    // NOTE: COMMIT can fail (e.g. deferred constraint violation, network
    // hiccup mid-acknowledge). On failure we throw so the caller's catch
    // (which we already have everywhere) maps to a typed error -- as
    // opposed to silently returning success and letting the worker LREM
    // the proc entry / clear the payload on data that was never actually
    // committed.
    struct CommitAwaiter {
        TxPtr tx;
        bool committed_ok = false;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            tx->setCommitCallback([this, h](bool ok) mutable {
                committed_ok = ok;
                h.resume();
            });
            tx.reset();  // trigger auto-commit -> callback fires when PG acks
        }

        bool await_resume() const noexcept { return committed_ok; }
    };

    const bool ok = co_await CommitAwaiter{std::move(tx_)};
    if (!ok) {
        spdlog::error("[uow] COMMIT was rejected by PostgreSQL");
        throw drogon::orm::Failure(
            "wikore::postgres::UnitOfWork: COMMIT was rejected by PostgreSQL");
    }
    committed_ = true;
}

UnitOfWork::~UnitOfWork() {
    if (!committed_ && tx_) {
        spdlog::warn("[uow] destroyed without commit - rolling back");
        tx_->rollback();
    }
}

} // namespace wikore::postgres
