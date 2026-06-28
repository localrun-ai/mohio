#include <catch2/catch_test_macros.hpp>
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/scheduler/partition_maintainer.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <cstdlib>
#include <string>

// Integration tests for V031 SECURITY DEFINER helpers and PartitionMaintainer.
//
// DrogonLoop is owned by test_promote_version.cpp; this file shares it.
// Tests skip when DATABASE_URL is not set.
//
// Invariants verified:
//   - wikore_app has EXECUTE; PUBLIC does not.
//   - Bounds are UTC-pinned (no DST shift).
//   - pg_inherits check distinguishes attached partitions from standalone tables.
//   - Functions raise on invalid inputs.
//   - C++ class run_once() counter reflects only committed creations.

namespace {

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

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

} // namespace

// ---------------------------------------------------------------------------
// wikore_ensure_audit_log_partition
// ---------------------------------------------------------------------------

TEST_CASE("V031 audit_log: creates new quarterly partition", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        auto r = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2041, 3);
        REQUIRE(r[0]["created"].as<bool>());
        // Rollback: PostgreSQL DDL is transactional; the new partition is removed.
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: idempotent - second call returns FALSE", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

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

TEST_CASE("V031 audit_log: UTC bounds for Q1", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2042, 1);
        auto r = co_await uow.exec(
            "SELECT pg_get_expr(c.relpartbound, c.oid) AS bound "
            "FROM   pg_class c "
            "JOIN   pg_namespace n ON n.oid = c.relnamespace "
            "WHERE  c.relname = 'audit_log_2042_q1' AND n.nspname = 'public'");
        REQUIRE_FALSE(r.empty());
        std::string bound = r[0]["bound"].as<std::string>();
        // Bounds must be Jan 1 and Apr 1 2042 at midnight UTC,
        // not DST-shifted (e.g. not 13:00 for NZDT+13).
        REQUIRE(bound.find("2042-01-01") != std::string::npos);
        REQUIRE(bound.find("2042-04-01") != std::string::npos);
        REQUIRE(bound.find("13:00") == std::string::npos);
        REQUIRE(bound.find("11:00") == std::string::npos);
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: UTC bounds for Q4 (year rollover)", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2042, 4);
        auto r = co_await uow.exec(
            "SELECT pg_get_expr(c.relpartbound, c.oid) AS bound "
            "FROM   pg_class c "
            "JOIN   pg_namespace n ON n.oid = c.relnamespace "
            "WHERE  c.relname = 'audit_log_2042_q4' AND n.nspname = 'public'");
        REQUIRE_FALSE(r.empty());
        std::string bound = r[0]["bound"].as<std::string>();
        REQUIRE(bound.find("2042-10-01") != std::string::npos);
        REQUIRE(bound.find("2043-01-01") != std::string::npos);
        REQUIRE(bound.find("13:00") == std::string::npos);
        uow.rollback();
    }());
}

TEST_CASE("V031 audit_log: invalid quarter raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    bool threw = false;
    try {
        exec_sync(db,
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2040, 5);
    } catch (const drogon::orm::DrogonDbException&) {
        threw = true;
    }
    REQUIRE(threw);
}

TEST_CASE("V031 audit_log: out-of-range year raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    bool threw = false;
    try {
        exec_sync(db,
            "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2023, 1);
    } catch (const drogon::orm::DrogonDbException&) {
        threw = true;
    }
    REQUIRE(threw);
}

TEST_CASE("V031 audit_log: conflict with non-partition table raises error", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // The pg_inherits check must NOT accept a standalone table as a valid
    // partition. When a standalone table occupies the name the SECURITY
    // DEFINER's EXECUTE DDL must fail, surfacing the conflict immediately
    // rather than silently returning FALSE (which would leave the partition
    // absent and rows overflowing to the default).
    bool threw = false;
    drogon::sync_wait([&db, &threw]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec("CREATE TABLE public.audit_log_2043_q2 (id BIGINT)");
        try {
            co_await uow.exec(
                "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2043, 2);
        } catch (const drogon::orm::DrogonDbException&) {
            threw = true;
        }
        uow.rollback();
    }());
    REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// wikore_ensure_usage_events_partition
// ---------------------------------------------------------------------------

TEST_CASE("V031 usage_events: creates new monthly partition", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

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
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

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

TEST_CASE("V031 usage_events: UTC bounds for December (year rollover)", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "SELECT public.wikore_ensure_usage_events_partition($1, $2)", 2042, 12);
        auto r = co_await uow.exec(
            "SELECT pg_get_expr(c.relpartbound, c.oid) AS bound "
            "FROM   pg_class c "
            "JOIN   pg_namespace n ON n.oid = c.relnamespace "
            "WHERE  c.relname = 'usage_events_2042_12' AND n.nspname = 'public'");
        REQUIRE_FALSE(r.empty());
        std::string bound = r[0]["bound"].as<std::string>();
        REQUIRE(bound.find("2042-12-01") != std::string::npos);
        REQUIRE(bound.find("2043-01-01") != std::string::npos);
        REQUIRE(bound.find("13:00") == std::string::npos);
        uow.rollback();
    }());
}

TEST_CASE("V031 usage_events: invalid month raises exception", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    bool threw = false;
    try {
        exec_sync(db,
            "SELECT public.wikore_ensure_usage_events_partition($1, $2)", 2040, 13);
    } catch (const drogon::orm::DrogonDbException&) {
        threw = true;
    }
    REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// Role / privilege tests
// ---------------------------------------------------------------------------

TEST_CASE("V031 wikore_app: has EXECUTE on partition functions", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // SET LOCAL ROLE ensures the function sees wikore_app as the current role;
    // the SECURITY DEFINER body still runs as the function owner (migration role)
    // but EXECUTE permission is checked against wikore_app.
    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec("SET LOCAL ROLE wikore_app");
        auto r = co_await uow.exec(
            "SELECT public.wikore_ensure_audit_log_partition($1, $2) AS created",
            2044, 1);
        REQUIRE(r[0]["created"].as<bool>());
        uow.rollback();
    }());
}

TEST_CASE("V031 wikore_app: has EXECUTE on overflow check", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec("SET LOCAL ROLE wikore_app");
        auto rows = co_await uow.exec(
            "SELECT partition_table, has_rows "
            "FROM   public.wikore_check_partition_overflow()");
        REQUIRE_FALSE(rows.empty());
        uow.rollback();
    }());
}

TEST_CASE("V031 unauthorized role: cannot call partition functions", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // Use a throwaway role that has no grants from V031.
    try {
        exec_sync(db, "CREATE ROLE wikore_partition_test_noaccess NOLOGIN");
    } catch (const drogon::orm::DrogonDbException&) {
        // Role already exists from a previous partial run.
    }

    bool denied = false;
    drogon::sync_wait([&db, &denied]() -> drogon::Task<void> {
        try {
            auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
            co_await uow.exec("SET LOCAL ROLE wikore_partition_test_noaccess");
            co_await uow.exec(
                "SELECT public.wikore_ensure_audit_log_partition($1, $2)", 2044, 2);
        } catch (const drogon::orm::DrogonDbException& ex) {
            std::string msg = ex.base().what();
            denied = msg.find("permission denied") != std::string::npos;
        }
    }());
    REQUIRE(denied);

    try {
        exec_sync(db, "DROP ROLE wikore_partition_test_noaccess");
    } catch (const drogon::orm::DrogonDbException&) {}
}

// ---------------------------------------------------------------------------
// wikore_check_partition_overflow
// ---------------------------------------------------------------------------

TEST_CASE("V031 overflow check: clean state - no rows in default partitions", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    auto rows = exec_sync(db,
        "SELECT partition_table, has_rows "
        "FROM   public.wikore_check_partition_overflow()");
    REQUIRE(rows.size() == 2);
    for (const auto& row : rows)
        REQUIRE_FALSE(row["has_rows"].as<bool>());
}

TEST_CASE("V031 overflow check: detects rows in audit_log_default", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        // 2024-06-01 falls before the earliest named partition (2026-04-01)
        // so the row routes to audit_log_default.
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
// C++ PartitionMaintainer
// ---------------------------------------------------------------------------

TEST_CASE("PartitionMaintainer::run_once: succeeds when all partitions exist", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // The default lookahead (4q / 12m) from 2026-Q2 / 2026-06 covers
    // exactly the partitions pre-created by V007 and V016, so run_once()
    // calls the SQL helpers which return FALSE for each (already attached).
    // Counter must reflect 0 actual creations.
    wikore::scheduler::PartitionMaintainer pm(
        db,
        [] { return false; },
        wikore::scheduler::PartitionMaintainer::Options{});

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
    }());

    REQUIRE(pm.partitions_created() == 0);
}

TEST_CASE("PartitionMaintainer::run_once: second call does not increase counter", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::scheduler::PartitionMaintainer pm(
        db,
        [] { return false; },
        wikore::scheduler::PartitionMaintainer::Options{});

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
        co_await pm.run_once();
    }());

    // Both sweeps found all partitions present; counter is still 0 after two runs.
    REQUIRE(pm.partitions_created() == 0);
}

TEST_CASE("PartitionMaintainer::run_once: creates and counts one new partition", "[integration][partition]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // V007 pre-creates audit_log Q2-Q4 2026 and Q1-Q2 2027 (five quarters).
    // A lookahead of 6 from Q2-2026 reaches Q3-2027, which V007 does not include.
    // V016 pre-creates usage_events 2026-06 through 2027-06 (twelve months),
    // which is exactly the default usage_events_lookahead=12, so 0 monthly
    // partitions need creating.  Net expected creations: 1.
    wikore::scheduler::PartitionMaintainer pm(
        db,
        [] { return false; },
        wikore::scheduler::PartitionMaintainer::Options{
            .interval               = std::chrono::hours(24),
            .audit_log_lookahead    = 6,
            .usage_events_lookahead = 12,
        });

    drogon::sync_wait([&pm]() -> drogon::Task<void> {
        co_await pm.run_once();
    }());

    REQUIRE(pm.partitions_created() == 1);

    // Detach and drop the newly created 2027-Q3 partition so the schema
    // stays consistent for subsequent test runs.
    exec_sync(db,
        "ALTER TABLE public.audit_log "
        "DETACH PARTITION public.audit_log_2027_q3");
    exec_sync(db, "DROP TABLE public.audit_log_2027_q3");
}
