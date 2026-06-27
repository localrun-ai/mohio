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

    committed_ = true;
    co_await CommitAwaiter{std::move(tx_)};
}

UnitOfWork::~UnitOfWork() {
    if (!committed_ && tx_) {
        spdlog::warn("[uow] destroyed without commit - rolling back");
        tx_->rollback();
    }
}

} // namespace wikore::postgres
