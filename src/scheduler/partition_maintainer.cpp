#include "wikore/scheduler/partition_maintainer.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <format>

namespace wikore::scheduler {

namespace {

drogon::Task<void> co_sleep_hrs(std::chrono::hours h)
{
    auto* loop = drogon::app().getLoop();
    struct Awaiter {
        trantor::EventLoop* loop;
        double              seconds;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> hdl) {
            loop->runAfter(seconds, [hdl]() mutable { hdl.resume(); });
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{loop, static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(h).count())};
}

// Returns the current UTC year and month as integers.
std::pair<int,int> utc_year_month()
{
    using namespace std::chrono;
    auto ymd = year_month_day{floor<days>(system_clock::now())};
    return {static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month())};
}

} // namespace

PartitionMaintainer::PartitionMaintainer(drogon::orm::DbClientPtr db,
                                         ShutdownPredicate        shutdown_requested,
                                         Options                  opts)
    : db_(std::move(db))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{}

// ---------------------------------------------------------------------------
// audit_log - quarterly partitions
//
// Naming:  audit_log_YYYY_qN   (N in {1,2,3,4})
// Quarter: Q1=Jan-Mar, Q2=Apr-Jun, Q3=Jul-Sep, Q4=Oct-Dec
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::ensure_audit_log_partitions()
{
    auto [year, month] = utc_year_month();
    int quarter = (month - 1) / 3 + 1;

    for (int i = 0; i < opts_.audit_log_lookahead; ++i) {
        int q = quarter + i;
        int y = year + (q - 1) / 4;
        q     = ((q - 1) % 4) + 1;

        int start_month = (q - 1) * 3 + 1;
        int end_month   = start_month + 3;
        int end_year    = y;
        if (end_month > 12) { end_month -= 12; ++end_year; }

        std::string name  = std::format("audit_log_{}_q{}", y, q);
        std::string from  = std::format("{}-{:02d}-01", y, start_month);
        std::string to    = std::format("{}-{:02d}-01", end_year, end_month);
        std::string sql   = std::format(
            "CREATE TABLE IF NOT EXISTS {} "
            "PARTITION OF audit_log FOR VALUES FROM ('{}') TO ('{}')",
            name, from, to);

        try {
            co_await db_->execSqlCoro(sql);
            spdlog::debug("[partition-maintainer] ensured {}", name);
            partitions_created_.fetch_add(1);
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[partition-maintainer] failed to create {}: {}",
                          name, ex.base().what());
        }
    }
}

// ---------------------------------------------------------------------------
// usage_events - monthly partitions
//
// Naming:  usage_events_YYYY_MM
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::ensure_usage_events_partitions()
{
    auto [year, month] = utc_year_month();

    for (int i = 0; i < opts_.usage_events_lookahead; ++i) {
        int m = month + i;
        int y = year + (m - 1) / 12;
        m     = ((m - 1) % 12) + 1;

        int next_m = m + 1, next_y = y;
        if (next_m > 12) { next_m = 1; ++next_y; }

        std::string name = std::format("usage_events_{}_{:02d}", y, m);
        std::string from = std::format("{}-{:02d}-01", y, m);
        std::string to   = std::format("{}-{:02d}-01", next_y, next_m);
        std::string sql  = std::format(
            "CREATE TABLE IF NOT EXISTS {} "
            "PARTITION OF usage_events FOR VALUES FROM ('{}') TO ('{}')",
            name, from, to);

        try {
            co_await db_->execSqlCoro(sql);
            spdlog::debug("[partition-maintainer] ensured {}", name);
            partitions_created_.fetch_add(1);
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[partition-maintainer] failed to create {}: {}",
                          name, ex.base().what());
        }
    }
}

// ---------------------------------------------------------------------------
// Check for rows in the catch-all DEFAULT partitions. Any row there means
// a named partition was missing at INSERT time. Log CRITICAL so an alert
// fires; the ops team must detach default, move rows, and re-attach.
// ---------------------------------------------------------------------------

drogon::Task<void> PartitionMaintainer::check_default_overflow()
{
    constexpr auto kCheckSql =
        "SELECT EXISTS(SELECT 1 FROM {} LIMIT 1) AS has_rows";

    for (const char* tbl : {"audit_log_default", "usage_events_default"}) {
        try {
            auto rows = co_await db_->execSqlCoro(
                std::format(kCheckSql, tbl));
            if (!rows.empty() && rows[0]["has_rows"].as<bool>()) {
                spdlog::critical(
                    "[partition-maintainer] OVERFLOW DETECTED: rows exist in {} "
                    "- a named partition was missing at insert time. "
                    "Manual intervention required: DETACH, move rows, ATTACH.",
                    tbl);
            }
        } catch (const drogon::orm::DrogonDbException& ex) {
            spdlog::error("[partition-maintainer] default-overflow check failed "
                          "for {}: {}", tbl, ex.base().what());
        }
    }
}

drogon::Task<void> PartitionMaintainer::run_once()
{
    spdlog::info("[partition-maintainer] sweep: audit_log lookahead={}q, "
                 "usage_events lookahead={}m",
                 opts_.audit_log_lookahead, opts_.usage_events_lookahead);
    co_await ensure_audit_log_partitions();
    co_await ensure_usage_events_partitions();
    co_await check_default_overflow();
}

drogon::Task<void> PartitionMaintainer::run()
{
    spdlog::info("[partition-maintainer] starting; interval={}h",
                 opts_.interval.count());
    while (!shutdown_()) {
        co_await run_once();
        if (shutdown_()) break;
        co_await co_sleep_hrs(opts_.interval);
    }
    spdlog::info("[partition-maintainer] stopped");
}

} // namespace wikore::scheduler
