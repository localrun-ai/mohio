#pragma once
#include "wikore/domain/types.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>

namespace wikore::postgres {

// ---------------------------------------------------------------------------
// UnitOfWork
//
// Wraps a Drogon Transaction. commit() is an async Task that uses
// Transaction::setCommitCallback to await PG acknowledgment before returning,
// ensuring callers see committed data on subsequent queries.
//
// Pattern:
//   auto uow = co_await UnitOfWork::begin(db);
//   co_await uow.exec("INSERT INTO ...", args...);
//   co_await uow.exec("INSERT INTO ...", args...);
//   co_await uow.commit();   // awaits PG COMMIT acknowledgment
//   // OR
//   uow.rollback();          // synchronous; calls tx_->rollback() then releases
//
// If the UoW goes out of scope without commit() being called the destructor
// logs a warning and calls rollback().
// ---------------------------------------------------------------------------

class UnitOfWork {
public:
    using TxPtr = std::shared_ptr<drogon::orm::Transaction>;

    static drogon::Task<UnitOfWork> begin(drogon::orm::DbClientPtr db);

    // Execute a parameterized SQL statement inside this transaction.
    // Args are DECAYED (taken by value into the coroutine frame) to
    // avoid the Drogon-lazy-Task forwarding-reference dangle bug: a
    // caller writing `co_await uow.exec("...", std::string(x))` would
    // bind the temporary string to a forwarding reference, which is
    // destroyed at the end of the call expression -- but Drogon
    // suspends BEFORE the body runs, so the body would read a freed
    // string. Decaying to values stores owned copies in the frame.
    //
    // Throws drogon::orm::DrogonDbException on error; caller wraps with
    // postgres::map_db_exception() for typed error values.
    template<typename... Args>
    drogon::Task<drogon::orm::Result>
    exec(std::string sql, Args... args) {
        co_return co_await tx_->execSqlCoro(
            std::move(sql), std::move(args)...);
    }

    // Await PG COMMIT acknowledgment. THROWS drogon::orm::DrogonDbException
    // (synthetic via setCommitCallback ok=false) when COMMIT fails so the
    // caller can map the error rather than seeing a silent partial-success.
    drogon::Task<void> commit();

    // Explicitly roll back and release the connection.
    void rollback() {
        if (tx_) {
            tx_->rollback();
            tx_.reset();
        }
    }

    UnitOfWork(const UnitOfWork&) = delete;
    UnitOfWork& operator=(const UnitOfWork&) = delete;
    UnitOfWork(UnitOfWork&&) = default;
    UnitOfWork& operator=(UnitOfWork&&) = default;

    ~UnitOfWork();

private:
    explicit UnitOfWork(TxPtr tx) : tx_(std::move(tx)) {}

    TxPtr tx_;
    bool  committed_ = false;
};

} // namespace wikore::postgres
