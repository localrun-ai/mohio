#include <catch2/catch_test_macros.hpp>
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/scheduler/partition_maintainer.hpp"
#include "wikore/db.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <utility>

// Integration tests for V031 SECURITY DEFINER helpers and PartitionMaintainer.
//
// DrogonLoop is owned by test_promote_version.cpp; this file shares it.
//
// Required env vars:
//   DATABASE_URL     - superuser/migration-owner connection (wikore)
//   PARTITION_DATABASE_URL - restricted partition-maintenance connection
//
// Invariants verified:
//   - The restricted login can call the functions but cannot read tables or
//     issue DDL directly.
//   - Bounds are UTC-pinned: the caller session timezone is set to Auckland
//     (UTC+12/13) BEFORE partition creation so the entire code path under test
//     runs with a non-UTC local clock.  Rows at UTC midnight land in the named
//     partition, proving make_timestamptz pinning works.
//   - pg_inherits + bounds check: an attached partition with mismatched range
//     raises an exception rather than silently returning FALSE.
//   - C++ PartitionMaintainer tests use partition_db() throughout.
//   - A timer-armed hook makes the shutdown-during-sleep test deterministic.

namespace {

bool db_available() {
    return std::getenv("DATABASE_URL") && std::getenv("PARTITION_DATABASE_URL");
}

// Superuser connection (wikore) - used for schema DDL and cleanup.
drogon::orm::DbClientPtr admin_db() { return wikore::Db::get(); }

// Non-superuser connection with only the V031 maintenance-function grants.
// Created once; the event loop is already running when tests start.
drogon::orm::DbClientPtr partition_db() {
    const char* url = std::getenv("PARTITION_DATABASE_URL");
    if (!url) return nullptr;
    static auto client = drogon::orm::DbClient::newPgClient(url, 4);
    return client;
}

template<typename... Args>
drogon::orm::Result exec_sync(
    drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

// Build PartitionMaintainer::Options with a fixed clock and configurable
// lookahead / sleep_chunk.  Use far-future years (2041+) to avoid collisions
// with partitions pre-created by V007/V016.
wikore::scheduler::PartitionMaintainer::Options fixed_opts(
    int year, int month,
    int audit_q    = 1,
    int usage_m    = 0,
    std::chrono::milliseconds sleep_chunk = std::chrono::seconds(30))
{
    return {
        .interval               = std::chrono::hours(24),
        .audit_log_lookahead    = audit_q,
        .usage_events_lookahead = usage_m,
        .sleep_chunk            = sleep_chunk,
        .now_utc_year_month     = [year, month] { return std::pair{year, month}; },
    };
}

} // namespace

// ---------------------------------------------------------------------------
// wikore_ensure_audit_log_partition
// ---------------------------------------------------------------------------

TEST_CASE("V031 audit_log: creates new quarterly partition", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2041, 3);
        REQUIRE(r[0]["created"].as<bool>());
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: idempotent - second call in same txn returns FALSE", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r1 = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2041, 4);
        REQUIRE(r1[0]["created"].as<bool>());
        auto r2 = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2041, 4);
        REQUIRE_FALSE(r2[0]["created"].as<bool>());
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: UTC bounds hold under Auckland caller timezone", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    // Set the session timezone to Auckland (UTC+12/+13) BEFORE creating the
    // partition so the entire code path runs with a non-UTC caller clock.
    // If make_timestamptz did not pin to UTC, bounds would be 12-13 h off and
    // the row at UTC midnight would fall into a different partition.
    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec("SET LOCAL TIME ZONE 'Pacific/Auckland'");
        co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2042, 1);

        // Row at exactly the UTC lower bound must land in audit_log_2042_q1.
        co_await uow.exec(
            "INSERT INTO public.audit_log (company_id, action, detail, created_at) "
            "VALUES ('00000000-0000-0000-0000-000000000000',"
            "        'test.utc_q1', '{}'::jsonb, '2042-01-01 00:00:00+00')");

        auto r = co_await uow.exec(
            "SELECT tableoid::regclass::text AS part "
            "FROM   public.audit_log "
            "WHERE  action = 'test.utc_q1' "
            "  AND  company_id = '00000000-0000-0000-0000-000000000000'");
        REQUIRE_FALSE(r.empty());
        REQUIRE(std::string(r[0]["part"].c_str()) == "audit_log_2042_q1");
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: UTC bounds hold for Q4 year rollover", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec("SET LOCAL TIME ZONE 'Pacific/Auckland'");
        co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2042, 4);

        co_await uow.exec(
            "INSERT INTO public.audit_log (company_id, action, detail, created_at) "
            "VALUES ('00000000-0000-0000-0000-000000000000',"
            "        'test.utc_q4', '{}'::jsonb, '2042-10-01 00:00:00+00')");

        auto r = co_await uow.exec(
            "SELECT tableoid::regclass::text AS part "
            "FROM   public.audit_log "
            "WHERE  action = 'test.utc_q4' "
            "  AND  company_id = '00000000-0000-0000-0000-000000000000'");
        REQUIRE_FALSE(r.empty());
        REQUIRE(std::string(r[0]["part"].c_str()) == "audit_log_2042_q4");
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: attached partition with wrong bounds raises", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    bool threw = false;
    drogon::sync_wait([&db, &threw]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        // Create a valid partition but with Q2 bounds instead of Q1 bounds.
        // This simulates a reclaimed or incorrectly ranged partition.
        co_await uow.exec(
            "CREATE TABLE public.audit_log_2042_q1 "
            "PARTITION OF public.audit_log "
            "FOR VALUES FROM ('2042-04-01 00:00:00+00') "
            "             TO ('2042-07-01 00:00:00+00')");
        try {
            // The function sees a pg_inherits-attached partition but wrong bounds.
            co_await uow.exec(
                "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2042, 1);
        } catch (const drogon::orm::DrogonDbException& ex) {
            std::string msg = ex.base().what();
            threw = msg.find("wrong bounds") != std::string::npos;
        }
        uow.rollback();
    }());
    REQUIRE(threw);
}

TEST_CASE("V031 audit_log: conflict with non-partition table raises", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    bool threw = false;
    drogon::sync_wait([&db, &threw]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec("CREATE TABLE public.audit_log_2043_q2 (id BIGINT)");
        try {
            co_await uow.exec(
                "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2043, 2);
        } catch (const drogon::orm::DrogonDbException&) { threw = true; }
        uow.rollback();
    }());
    REQUIRE(threw);
}

TEST_CASE("V031 audit_log: invalid quarter raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    bool threw = false;
    try {
        exec_sync(admin_db(),
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2040, 5);
    } catch (const drogon::orm::DrogonDbException&) { threw = true; }
    REQUIRE(threw);
}

TEST_CASE("V031 audit_log: out-of-range year raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    bool threw = false;
    try {
        exec_sync(admin_db(),
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2023, 1);
    } catch (const drogon::orm::DrogonDbException&) { threw = true; }
    REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// wikore_ensure_usage_events_partition
// ---------------------------------------------------------------------------

TEST_CASE("V031 usage_events: creates new monthly partition", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await uow.exec(
            "SELECT public.wikore_ensure_usage_events_partition($1, $2) AS created",
            2041, 8);
        REQUIRE(r[0]["created"].as<bool>());
        uow.rollback();
    }());
}

TEST_CASE("V031 usage_events: idempotent - second call returns FALSE", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r1 = co_await uow.exec(
            "SELECT public.wikore_ensure_usage_events_partition($1, $2) AS created",
            2041, 9);
        REQUIRE(r1[0]["created"].as<bool>());
        auto r2 = co_await uow.exec(
            "SELECT public.wikore_ensure_usage_events_partition($1, $2) AS created",
            2041, 9);
        REQUIRE_FALSE(r2[0]["created"].as<bool>());
        uow.rollback();
    }());
}

TEST_CASE("V031 usage_events: UTC bounds hold under Auckland caller timezone", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec("SET LOCAL TIME ZONE 'Pacific/Auckland'");
        co_await uow.exec(
            "SELECT public.wikore_ensure_usage_events_partition($1, $2)", 2042, 12);

        co_await uow.exec(
            "INSERT INTO public.usage_events "
            "  (company_id, event_type, model_name, created_at) "
            "VALUES ('00000000-0000-0000-0000-000000000000',"
            "        'llm_embed', 'test-model', '2042-12-01 00:00:00+00')");

        auto r = co_await uow.exec(
            "SELECT tableoid::regclass::text AS part "
            "FROM   public.usage_events "
            "WHERE  company_id = '00000000-0000-0000-0000-000000000000' "
            "  AND  event_type = 'llm_embed'");
        REQUIRE_FALSE(r.empty());
        REQUIRE(std::string(r[0]["part"].c_str()) == "usage_events_2042_12");
        uow.rollback();
    }());
}

TEST_CASE("V031 usage_events: invalid month raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    bool threw = false;
    try {
        exec_sync(admin_db(),
            "SELECT public.wikore_ensure_usage_events_partition($1, $2)", 2040, 13);
    } catch (const drogon::orm::DrogonDbException&) { threw = true; }
    REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// Role / privilege: non-superuser connection via PARTITION_DATABASE_URL
// ---------------------------------------------------------------------------

TEST_CASE("V031 wikore_partition_test_login: can execute partition functions", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto adb = partition_db();
    REQUIRE(adb != nullptr);

    // EXECUTE is inherited only from wikore_partition_maintainer, not from the
    // application or migration role.
    drogon::sync_wait([&adb]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(adb);
        auto r = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2044, 1);
        REQUIRE(r[0]["created"].as<bool>());
        uow.rollback();
    }());
}

TEST_CASE("V031 wikore_partition_test_login: can call overflow check", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto adb = partition_db();
    REQUIRE(adb != nullptr);

    drogon::sync_wait([&adb]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(adb);
        auto rows = co_await uow.exec(
            "SELECT partition_table, has_rows "
            "FROM   public.wikore_check_partition_overflow()");
        REQUIRE_FALSE(rows.empty());
        uow.rollback();
    }());
}

TEST_CASE("V031 partition login: cannot read tables or issue DDL directly", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = partition_db();

    bool select_denied = false;
    try {
        exec_sync(db, "SELECT 1 FROM public.audit_log_default LIMIT 1");
    } catch (const drogon::orm::DrogonDbException& ex) {
        select_denied = std::string(ex.base().what()).find("permission denied") !=
                        std::string::npos;
    }

    bool ddl_denied = false;
    try {
        exec_sync(db, "CREATE TABLE public.partition_privilege_escape(id INT)");
    } catch (const drogon::orm::DrogonDbException& ex) {
        ddl_denied = std::string(ex.base().what()).find("permission denied") !=
                     std::string::npos;
    }
    if (!ddl_denied) {
        exec_sync(admin_db(),
                  "DROP TABLE IF EXISTS public.partition_privilege_escape");
    }

    REQUIRE(select_denied);
    REQUIRE(ddl_denied);
}

TEST_CASE("V031 unauthorized role: cannot call partition functions", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    try { exec_sync(db, "CREATE ROLE wikore_partition_test_noaccess NOLOGIN"); }
    catch (const drogon::orm::DrogonDbException&) {}
    exec_sync(db,
              "GRANT USAGE ON SCHEMA public TO wikore_partition_test_noaccess");

    bool denied = false;
    drogon::sync_wait([&db, &denied]() -> drogon::Task<void> {
        try {
            auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
            co_await uow.exec("SET LOCAL ROLE wikore_partition_test_noaccess");
            co_await uow.exec(
                "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2044, 2);
        } catch (const drogon::orm::DrogonDbException& ex) {
            std::string msg = ex.base().what();
            denied = msg.find(
                "permission denied for function "
                "wikore_ensure_audit_log_partition") != std::string::npos;
        }
    }());
    REQUIRE(denied);

    exec_sync(db,
              "REVOKE USAGE ON SCHEMA public FROM wikore_partition_test_noaccess");
    try { exec_sync(db, "DROP ROLE wikore_partition_test_noaccess"); }
    catch (const drogon::orm::DrogonDbException&) {}
}

// ---------------------------------------------------------------------------
// wikore_check_partition_overflow
// ---------------------------------------------------------------------------

TEST_CASE("V031 overflow check: clean state - default partitions are empty", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");

    auto rows = exec_sync(admin_db(),
        "SELECT partition_table, has_rows "
        "FROM   public.wikore_check_partition_overflow()");
    REQUIRE(rows.size() == 2);
    for (const auto& row : rows)
        REQUIRE_FALSE(row["has_rows"].as<bool>());
}

TEST_CASE("V031 overflow check: detects rows in audit_log_default", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        // 2024-06-01 predates all named partitions; routes to audit_log_default.
        co_await uow.exec(
            "INSERT INTO public.audit_log (company_id, action, detail, created_at) "
            "VALUES ('00000000-0000-0000-0000-000000000000',"
            "        'test.overflow', '{}'::jsonb, '2024-06-01 00:00:00+00')");

        auto rows = co_await uow.exec(
            "SELECT partition_table, has_rows "
            "FROM   public.wikore_check_partition_overflow()");
        bool audit_overflow = false;
        for (const auto& row : rows) {
            if (std::string(row["partition_table"].c_str()) == "audit_log_default")
                audit_overflow = row["has_rows"].as<bool>();
        }
        REQUIRE(audit_overflow);
        uow.rollback();
    }());
}

// ---------------------------------------------------------------------------
// C++ PartitionMaintainer - uses partition_db() throughout
// ---------------------------------------------------------------------------

TEST_CASE("PartitionMaintainer::run_once: creates exactly one partition (fixed clock)", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto adb = partition_db();

    wikore::scheduler::PartitionMaintainer pm(
        adb, [] { return false; },
        fixed_opts(2042, 4, 1, 0));  // Q2 2042; only 1 audit_log quarter requested

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
    }());

    REQUIRE(pm.partitions_created() == 1);

    exec_sync(admin_db(),
        "ALTER TABLE public.audit_log DETACH PARTITION public.audit_log_2042_q2");
    exec_sync(admin_db(), "DROP TABLE public.audit_log_2042_q2");
}

TEST_CASE("PartitionMaintainer::run_once: second run is idempotent (counter unchanged)", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto adb = partition_db();

    wikore::scheduler::PartitionMaintainer pm(
        adb, [] { return false; },
        fixed_opts(2042, 7, 1, 0));  // Q3 2042

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
        std::size_t after_first = pm.partitions_created();
        co_await pm.run_once();
        REQUIRE(pm.partitions_created() == after_first);
    }());

    exec_sync(admin_db(),
        "ALTER TABLE public.audit_log DETACH PARTITION public.audit_log_2042_q3");
    exec_sync(admin_db(), "DROP TABLE public.audit_log_2042_q3");
}

TEST_CASE("PartitionMaintainer::run_once: SQL failure keeps counter at zero", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto adb = partition_db();

    // A standalone table blocks the SECURITY DEFINER function; the outer catch
    // must absorb the error without incrementing the counter.
    exec_sync(admin_db(), "CREATE TABLE public.audit_log_2043_q1 (id BIGINT)");

    wikore::scheduler::PartitionMaintainer pm(
        adb, [] { return false; },
        fixed_opts(2043, 1, 1, 0));

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
    }());

    REQUIRE(pm.partitions_created() == 0);
    exec_sync(admin_db(), "DROP TABLE public.audit_log_2043_q1");
}

TEST_CASE("PartitionMaintainer::run_once: skips DDL when advisory lock is held", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto adb = partition_db();

    // Hold the xact-scoped advisory lock on one connection (admin_db).
    // run_once() on partition_db() will see the lock held, skip DDL, and count 0.
    drogon::sync_wait([&adb]() -> drogon::Task<void> {
        auto lock_tx = co_await admin_db()->newTransactionCoro();
        co_await lock_tx->execSqlCoro(
            "SELECT pg_advisory_xact_lock(hashtext('wikore-partition-maintainer'))");

        wikore::scheduler::PartitionMaintainer pm(
            adb, [] { return false; },
            fixed_opts(2042, 10, 1, 0));  // Q4 2042
        co_await pm.run_once();

        REQUIRE(pm.partitions_created() == 0);
        lock_tx->rollback();
    }());
    // audit_log_2042_q4 was never created (lock prevented it); no cleanup needed.
}

TEST_CASE("PartitionMaintainer::run: shutdown during sleep exits within sleep_chunk", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / PARTITION_DATABASE_URL not set");
    auto adb = partition_db();

    std::atomic<bool> g_shutdown{false};
    std::atomic<bool> sleep_was_armed{false};
    std::atomic_flag reported{};
    std::promise<void> armed_promise;
    auto armed = armed_promise.get_future();
    wikore::scheduler::PartitionMaintainer pm(
        adb,
        [&g_shutdown] { return g_shutdown.load(); },
        {
            .interval               = std::chrono::hours(24),
            .audit_log_lookahead    = 0,   // run_once() completes near-instantly
            .usage_events_lookahead = 0,
            .sleep_chunk            = std::chrono::milliseconds(100),
            .on_sleep_armed          = [&] {
                if (!reported.test_and_set()) armed_promise.set_value();
            },
            .now_utc_year_month     = [] { return std::pair{2026, 1}; },
        });

    // Signal only after co_sleep_ms has armed the event-loop timer. This proves
    // shutdown arrives while the coroutine is suspended in the sleep path.
    std::thread signaler([&] {
        sleep_was_armed.store(
            armed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        g_shutdown.store(true);
    });

    auto t0 = std::chrono::steady_clock::now();
    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run();
    }());
    auto elapsed = std::chrono::steady_clock::now() - t0;

    signaler.join();

    REQUIRE(sleep_was_armed.load());
    // The 2-second fallback prevents a broken hook from hanging the test.
    REQUIRE(elapsed < std::chrono::seconds(3));
    // run_once() with lookahead=0 creates no partitions; no cleanup needed.
}
