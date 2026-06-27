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
    // Throws drogon::orm::DrogonDbException on error; caller wraps with
    // postgres::map_db_exception() for typed error values.
    template<typename... Args>
    drogon::Task<drogon::orm::Result>
    exec(std::string sql, Args&&... args) {
        co_return co_await tx_->execSqlCoro(
            std::move(sql), std::forward<Args>(args)...);
    }

    // Await PG COMMIT acknowledgment before returning. Safe to call from
    // within a coroutine; the event loop is not blocked.
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
