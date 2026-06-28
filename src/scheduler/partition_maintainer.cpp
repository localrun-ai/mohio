#include "wikore/scheduler/partition_maintainer.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <format>

namespace wikore::scheduler {

namespace {

// Coroutine-friendly millisecond sleep using Drogon's event loop timer.
drogon::Task<void> co_sleep_ms(std::chrono::milliseconds d)
{
    auto* loop = drogon::app().getLoop();
    struct Awaiter {
        trantor::EventLoop*       loop;
        std::chrono::milliseconds delay;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) {
            loop->runAfter(static_cast<double>(delay.count()) / 1000.0,
                           [h]() mutable { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{loop, d};
}

// Returns current UTC year and month.
std::pair<int,int> utc_year_month()
{
    using namespace std::chrono;
    auto ymd = year_month_day{floor<days>(system_clock::now())};
    return {static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month())};
}

constexpr std::string_view kAdvisoryLockName = "wikore-partition-maintainer";
// Wake every 30s during the inter-sweep sleep to check shutdown_.
constexpr auto kSleepChunk = std::chrono::seconds(30);

} // namespace

PartitionMaintainer::PartitionMaintainer(drogon::orm::DbClientPtr db,
                                         ShutdownPredicate        shutdown_requested,
                                         Options                  opts)
    : db_(std::move(db))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{}

// ---------------------------------------------------------------------------
// interruptible_sleep
//
// Sleeps for opts_.interval but wakes every kSleepChunk to check shutdown_.
// SIGTERM received at the start of the sleep returns in at most 30 seconds
// instead of up to 24 hours.
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::interruptible_sleep()
{
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(opts_.interval);

    while (!shutdown_()) {
        const auto now      = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto chunk =
            std::min(remaining,
                     std::chrono::duration_cast<std::chrono::milliseconds>(kSleepChunk));
        co_await co_sleep_ms(chunk);
    }
}

// ---------------------------------------------------------------------------
// check_default_overflow
//
// Run outside the advisory lock (read-only; any replica). Logs CRITICAL if
// any rows exist in the catch-all DEFAULT partitions so ops is alerted before
// the window for lossless recovery (detach/move/reattach) shrinks.
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::check_default_overflow()
{
    for (const char* tbl : {"audit_log_default", "usage_events_default"}) {
        try {
            auto rows = co_await db_->execSqlCoro(
                std::format("SELECT EXISTS(SELECT 1 FROM {} LIMIT 1) AS has_rows", tbl));
            if (!rows.empty() && rows[0]["has_rows"].as<bool>()) {
                spdlog::critical(
                    "[partition-maintainer] OVERFLOW: rows found in {} - "
                    "a named partition was missing at insert time. "
                    "Ops required: DETACH, move rows, ATTACH.",
                    tbl);
            }
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[partition-maintainer] overflow check failed for {}: {}",
                          tbl, ex.base().what());
        }
    }
}

// ---------------------------------------------------------------------------
// run_once
//
// Acquires a PostgreSQL advisory xact lock (pg_try_advisory_xact_lock) inside
// a UnitOfWork so only one scheduler replica performs DDL per cycle. Non-owners
// skip immediately. The owning replica calls wikore_ensure_partition() (V031
// SECURITY DEFINER) for each expected partition; the function returns TRUE only
// when a partition is actually created, so partitions_created_ is accurate.
// Default-overflow check runs after commit on all replicas (read-only).
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::run_once()
{
    spdlog::info("[partition-maintainer] sweep: audit_log lookahead={}q "
                 "usage_events lookahead={}m",
                 opts_.audit_log_lookahead, opts_.usage_events_lookahead);

    // --- Advisory lock + DDL phase ---
    int created = 0;
    try {
        auto uow = co_await postgres::UnitOfWork::begin(db_);

        auto lock_rows = co_await uow.exec(
            "SELECT pg_try_advisory_xact_lock(hashtext($1)) AS got",
            std::string(kAdvisoryLockName));
        const bool got_lock = !lock_rows.empty() && lock_rows[0]["got"].as<bool>();

        if (!got_lock) {
            spdlog::debug("[partition-maintainer] another replica holds the lock; skipping DDL");
            uow.rollback();
            goto overflow_check;
        }

        {
            auto [year, month] = utc_year_month();

            // audit_log: quarterly
            const int quarter = (month - 1) / 3 + 1;
            for (int i = 0; i < opts_.audit_log_lookahead; ++i) {
                int q = quarter + i;
                int y = year + (q - 1) / 4;
                q     = ((q - 1) % 4) + 1;

                int start_m = (q - 1) * 3 + 1;
                int end_m   = start_m + 3;
                int end_y   = y;
                if (end_m > 12) { end_m -= 12; ++end_y; }

                std::string name = std::format("audit_log_{}_q{}", y, q);
                std::string from = std::format("{}-{:02d}-01", y, start_m);
                std::string to   = std::format("{}-{:02d}-01", end_y, end_m);

                try {
                    auto r = co_await uow.exec(
                        "SELECT wikore_ensure_partition($1,$2,$3,$4) AS created",
                        std::string("audit_log"), name, from, to);
                    if (!r.empty() && r[0]["created"].as<bool>()) {
                        spdlog::info("[partition-maintainer] created {}", name);
                        ++created;
                    } else {
                        spdlog::debug("[partition-maintainer] already exists: {}", name);
                    }
                } catch (const drogon::orm::DrogonDbException& ex) {
                    spdlog::error("[partition-maintainer] failed to ensure {}: {}",
                                  name, ex.base().what());
                }
            }

            // usage_events: monthly
            for (int i = 0; i < opts_.usage_events_lookahead; ++i) {
                int m = month + i;
                int y = year + (m - 1) / 12;
                m     = ((m - 1) % 12) + 1;

                int next_m = m + 1, next_y = y;
                if (next_m > 12) { next_m = 1; ++next_y; }

                std::string name = std::format("usage_events_{}_{:02d}", y, m);
                std::string from = std::format("{}-{:02d}-01", y, m);
                std::string to   = std::format("{}-{:02d}-01", next_y, next_m);

                try {
                    auto r = co_await uow.exec(
                        "SELECT wikore_ensure_partition($1,$2,$3,$4) AS created",
                        std::string("usage_events"), name, from, to);
                    if (!r.empty() && r[0]["created"].as<bool>()) {
                        spdlog::info("[partition-maintainer] created {}", name);
                        ++created;
                    } else {
                        spdlog::debug("[partition-maintainer] already exists: {}", name);
                    }
                } catch (const drogon::orm::DrogonDbException& ex) {
                    spdlog::error("[partition-maintainer] failed to ensure {}: {}",
                                  name, ex.base().what());
                }
            }
        }

        if (auto r = co_await uow.commit(); !r)
            spdlog::error("[partition-maintainer] commit failed: {}", r.error().message);
        else
            partitions_created_.fetch_add(static_cast<std::size_t>(created));

        spdlog::info("[partition-maintainer] sweep complete; created={}", created);
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[partition-maintainer] sweep failed: {}", ex.base().what());
    }

overflow_check:
    co_await check_default_overflow();
}

drogon::Task<void> PartitionMaintainer::run()
{
    spdlog::info("[partition-maintainer] starting; interval={}h",
                 opts_.interval.count());
    while (!shutdown_()) {
        co_await run_once();
        if (shutdown_()) break;
        co_await interruptible_sleep();
    }
    spdlog::info("[partition-maintainer] stopped");
}

} // namespace wikore::scheduler
