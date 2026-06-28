#include <catch2/catch_test_macros.hpp>
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/scheduler/partition_maintainer.hpp"
#include "wikore/db.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <chrono>
#include <cstdlib>
#include <string>

// Integration tests for V031 SECURITY DEFINER helpers and PartitionMaintainer.
//
// DrogonLoop is owned by test_promote_version.cpp; this file shares it.
//
// Required env vars:
//   DATABASE_URL     - superuser/migration-owner connection (wikore)
//   DATABASE_URL_APP - non-superuser runtime connection (wikore_app_login)
//
// Both env vars must be present for [partition] tests to run.
//
// Invariants verified:
//   - wikore_app (via wikore_app_login) has EXECUTE; PUBLIC does not.
//   - Bounds are UTC-pinned: rows at UTC midnight land in the correct
//     partition even when the session timezone is set to Auckland (UTC+12/13).
//   - pg_inherits check rejects standalone tables: the function raises rather
//     than returning FALSE and leaving the partition absent.
//   - Invalid inputs (bad quarter, month, year) raise exceptions.
//   - Idempotency: second call in the same txn returns FALSE.
//   - Overflow detection reports rows in the default partition.
//   - C++ class run_once() with an injected clock and advisory-lock exclusion:
//       * actual creations are counted only after commit;
//       * a peer holding the lock causes the sweep to skip gracefully;
//       * a SQL failure rolls back cleanly with counter = 0;
//       * shutdown_() = true causes run() to exit promptly.

namespace {

bool db_available() {
    return std::getenv("DATABASE_URL") && std::getenv("DATABASE_URL_APP");
}

// Superuser connection (wikore) - used for schema operations.
drogon::orm::DbClientPtr admin_db() { return wikore::Db::get(); }

// Non-superuser connection (wikore_app_login inherits from wikore_app).
// Created once; safe because the event loop is running when tests run.
drogon::orm::DbClientPtr app_db() {
    const char* url = std::getenv("DATABASE_URL_APP");
    if (!url) return nullptr;
    static auto client = drogon::orm::DbClient::newPgClient(url, 2);
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

// PartitionMaintainer Options with a fixed clock and reduced lookahead.
// Uses a year/month where no partitions exist yet in the schema (2042+).
wikore::scheduler::PartitionMaintainer::Options fixed_clock_opts(
    int year, int month,
    int audit_q_lookahead    = 1,
    int usage_month_lookahead = 0)
{
    return wikore::scheduler::PartitionMaintainer::Options{
        .interval               = std::chrono::hours(24),
        .audit_log_lookahead    = audit_q_lookahead,
        .usage_events_lookahead = usage_month_lookahead,
        .now_utc_year_month     = [year, month] { return std::pair{year, month}; },
    };
}

} // namespace

// ---------------------------------------------------------------------------
// wikore_ensure_audit_log_partition: creation, idempotency, bounds, errors
// ---------------------------------------------------------------------------

TEST_CASE("V031 audit_log: creates new quarterly partition", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2041, 3);
        REQUIRE(r[0]["created"].as<bool>());
        uow.rollback();  // PostgreSQL DDL is transactional; partition is removed.
    }());
}

TEST_CASE("V031 audit_log: idempotent - second call in same txn returns FALSE", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
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

TEST_CASE("V031 audit_log: UTC bounds - rows land in partition under Auckland TZ", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    // This test proves the UTC-pinned invariant behaviorally:
    // Auckland is UTC+12 or UTC+13 (DST). If the partition bounds were computed
    // using the session timezone, the lower bound would be off by 12-13 hours,
    // and a row at exactly 2042-01-01 00:00:00 UTC would fall into a different
    // partition or the default. The row must land in audit_log_2042_q1.
    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2042, 1);

        // Stress-test: non-UTC session timezone.
        co_await uow.exec("SET LOCAL TIME ZONE 'Pacific/Auckland'");

        // Insert at exactly the UTC lower bound of Q1 2042.
        co_await uow.exec(
            "INSERT INTO public.audit_log (company_id, action, detail, created_at) "
            "VALUES ('00000000-0000-0000-0000-000000000000',"
            "        'test.utc_bounds', '{}'::jsonb, '2042-01-01 00:00:00+00')");

        auto r = co_await uow.exec(
            "SELECT tableoid::regclass::text AS part "
            "FROM   public.audit_log "
            "WHERE  action = 'test.utc_bounds' "
            "  AND  company_id = '00000000-0000-0000-0000-000000000000'");
        REQUIRE_FALSE(r.empty());
        // tableoid::regclass::text omits schema when the schema is in search_path.
        REQUIRE(std::string(r[0]["part"].c_str()) == "audit_log_2042_q1");
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: UTC bounds for Q4 year rollover", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2042, 4);
        co_await uow.exec("SET LOCAL TIME ZONE 'Pacific/Auckland'");

        // First timestamp in Q4 (Oct 1 2042 00:00 UTC) must land in the partition.
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

TEST_CASE("V031 audit_log: invalid quarter raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    bool threw = false;
    try {
        exec_sync(db,
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2040, 5);
    } catch (const drogon::orm::DrogonDbException&) { threw = true; }
    REQUIRE(threw);
}

TEST_CASE("V031 audit_log: out-of-range year raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    bool threw = false;
    try {
        exec_sync(db,
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2023, 1);
    } catch (const drogon::orm::DrogonDbException&) { threw = true; }
    REQUIRE(threw);
}

TEST_CASE("V031 audit_log: conflict with non-partition table raises error", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    // If a standalone table occupies the target name, pg_inherits returns FALSE
    // (not an attached partition), so the function attempts CREATE TABLE which
    // fails with "relation already exists". This surfaces the conflict instead
    // of silently returning FALSE and leaving the parent without a partition.
    bool threw = false;
    drogon::sync_wait([&db, &threw]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "CREATE TABLE public.audit_log_2043_q2 (id BIGINT)");
        try {
            co_await uow.exec(
                "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2043, 2);
        } catch (const drogon::orm::DrogonDbException&) { threw = true; }
        uow.rollback();
    }());
    REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// wikore_ensure_usage_events_partition
// ---------------------------------------------------------------------------

TEST_CASE("V031 usage_events: creates new monthly partition", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
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
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
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

TEST_CASE("V031 usage_events: UTC bounds for December year rollover", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "SELECT public.wikore_ensure_usage_events_partition($1, $2)", 2042, 12);
        co_await uow.exec("SET LOCAL TIME ZONE 'Pacific/Auckland'");

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
        // tableoid::regclass::text omits schema when schema is in search_path.
        REQUIRE(std::string(r[0]["part"].c_str()) == "usage_events_2042_12");
        uow.rollback();
    }());
}

TEST_CASE("V031 usage_events: invalid month raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    bool threw = false;
    try {
        exec_sync(db,
            "SELECT public.wikore_ensure_usage_events_partition($1, $2)", 2040, 13);
    } catch (const drogon::orm::DrogonDbException&) { threw = true; }
    REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// Role / privilege tests - using distinct non-superuser connection
// ---------------------------------------------------------------------------

TEST_CASE("V031 wikore_app_login: can execute partition functions", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    // wikore_app_login is NOSUPERUSER and inherits EXECUTE from wikore_app.
    // This proves the GRANT TO wikore_app is effective for a non-privileged login.
    auto adb = app_db();
    REQUIRE(adb != nullptr);

    drogon::sync_wait([&adb]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(adb);
        auto r = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2044, 1);
        REQUIRE(r[0]["created"].as<bool>());
        uow.rollback();
    }());
}

TEST_CASE("V031 wikore_app_login: can call overflow check", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto adb = app_db();
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

TEST_CASE("V031 unauthorized role: cannot call partition functions", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    // A role that has no membership in wikore_app gets permission denied.
    try {
        exec_sync(db, "CREATE ROLE wikore_partition_test_noaccess NOLOGIN");
    } catch (const drogon::orm::DrogonDbException&) {}

    bool denied = false;
    drogon::sync_wait([&db, &denied]() -> drogon::Task<void> {
        try {
            auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
            // SET LOCAL ROLE changes CURRENT_USER, which is what PG checks for
            // EXECUTE privilege. The non-superuser role has no EXECUTE grant.
            co_await uow.exec("SET LOCAL ROLE wikore_partition_test_noaccess");
            co_await uow.exec(
                "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2044, 2);
        } catch (const drogon::orm::DrogonDbException& ex) {
            std::string msg = ex.base().what();
            denied = msg.find("permission denied") != std::string::npos;
        }
    }());
    REQUIRE(denied);

    try { exec_sync(db, "DROP ROLE wikore_partition_test_noaccess"); }
    catch (const drogon::orm::DrogonDbException&) {}
}

// ---------------------------------------------------------------------------
// wikore_check_partition_overflow
// ---------------------------------------------------------------------------

TEST_CASE("V031 overflow check: clean state - default partitions are empty", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    auto rows = exec_sync(db,
        "SELECT partition_table, has_rows "
        "FROM   public.wikore_check_partition_overflow()");
    REQUIRE(rows.size() == 2);
    for (const auto& row : rows)
        REQUIRE_FALSE(row["has_rows"].as<bool>());
}

TEST_CASE("V031 overflow check: detects rows in audit_log_default", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        // 2024-06-01 falls before the earliest named partition so it routes
        // to audit_log_default.
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
// C++ PartitionMaintainer - clock injection and correctness
// ---------------------------------------------------------------------------

TEST_CASE("PartitionMaintainer::run_once: creates exactly one partition with fixed clock", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    // Clock pinned to April 2042 (Q2 2042). Only 1 audit_log quarter in the
    // lookahead (Q2 2042 itself) - this partition does not pre-exist. The
    // test is time-independent because the clock is injected, not system UTC.
    wikore::scheduler::PartitionMaintainer pm(
        db, [] { return false; },
        fixed_clock_opts(/*year=*/2042, /*month=*/4,
                         /*audit_q=*/1, /*usage_m=*/0));

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
    }());

    REQUIRE(pm.partitions_created() == 1);

    // Cleanup: detach and drop the created partition so the schema stays clean.
    exec_sync(db,
        "ALTER TABLE public.audit_log "
        "DETACH PARTITION public.audit_log_2042_q2");
    exec_sync(db, "DROP TABLE public.audit_log_2042_q2");
}

TEST_CASE("PartitionMaintainer::run_once: second run is idempotent (counter does not increase)", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    wikore::scheduler::PartitionMaintainer pm(
        db, [] { return false; },
        fixed_clock_opts(2042, 7, 1, 0));  // Q3 2042, lookahead 1

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
        std::size_t after_first = pm.partitions_created();
        co_await pm.run_once();
        REQUIRE(pm.partitions_created() == after_first);  // no new creations
    }());

    exec_sync(db,
        "ALTER TABLE public.audit_log "
        "DETACH PARTITION public.audit_log_2042_q3");
    exec_sync(db, "DROP TABLE public.audit_log_2042_q3");
}

TEST_CASE("PartitionMaintainer::run_once: SQL failure keeps counter at zero", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    // Pre-create a standalone table that will block the partition function,
    // causing it to raise "relation already exists". The outer catch in
    // run_once() must handle this without incrementing the counter.
    exec_sync(db, "CREATE TABLE public.audit_log_2043_q1 (id BIGINT)");

    wikore::scheduler::PartitionMaintainer pm(
        db, [] { return false; },
        fixed_clock_opts(2043, 1, 1, 0));  // Q1 2043, lookahead 1

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();  // function raises; outer catch absorbs it
    }());

    REQUIRE(pm.partitions_created() == 0);

    exec_sync(db, "DROP TABLE public.audit_log_2043_q1");
}

TEST_CASE("PartitionMaintainer::run_once: skips DDL when advisory lock is held", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    // Hold the advisory lock on a separate transaction. The pool has 8
    // connections so run_once() will get a different one and see
    // pg_try_advisory_xact_lock return FALSE immediately.
    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto lock_tx = co_await db->newTransactionCoro();
        co_await lock_tx->execSqlCoro(
            "SELECT pg_advisory_xact_lock(hashtext('wikore-partition-maintainer'))");

        wikore::scheduler::PartitionMaintainer pm(
            db, [] { return false; },
            fixed_clock_opts(2042, 10, 1, 0));  // Q4 2042
        co_await pm.run_once();

        // Lock was held by lock_tx; pm skipped DDL and counted nothing.
        REQUIRE(pm.partitions_created() == 0);

        lock_tx->rollback();
    }());
    // audit_log_2042_q4 was never created (pm skipped), so no cleanup needed.
}

TEST_CASE("PartitionMaintainer::run: exits immediately when shutdown is set before start", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL / DATABASE_URL_APP not set");
    auto db = admin_db();

    // run() checks !shutdown_() as the while-loop condition before calling
    // run_once(). With shutdown_()=true from the start the loop body is never
    // entered; run() logs "starting"/"stopped" and returns without any I/O.
    wikore::scheduler::PartitionMaintainer pm(
        db, [] { return true; },
        fixed_clock_opts(2041, 1, 1, 0));

    auto t0 = std::chrono::steady_clock::now();
    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run();
    }());
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // No DB round-trips: must complete well under 1 second.
    REQUIRE(elapsed < std::chrono::seconds(1));
    // run_once() was never called so no partition was created; no cleanup needed.
}
