#include <catch2/catch_test_macros.hpp>
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include "wikore/application/promote_document_version.hpp"
#include "wikore/db.hpp"
#include "wikore/config.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <thread>
#include <atomic>
#include <cstdlib>

// ---------------------------------------------------------------------------
// DB integration tests - Iteration 0 exit criteria.
//
// Requires DATABASE_URL env var. All tests SKIP when not set.
// The static DrogonLoop starts Drogon's event loop in a background thread.
// drogon::sync_wait() only accepts drogon::Task<T>, so all direct DB calls
// are wrapped in task() lambdas.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO  = "cafecafe-0000-0000-0000-000000000001";
constexpr auto OU  = "0a000000-0000-0000-0000-000000000001";
constexpr auto USR = "cafecafe-0000-0000-0000-000000000002";
constexpr auto DOC = "d0c00000-0000-0000-0000-000000000001";
constexpr auto V1  = "7e100001-0000-0000-0000-0000000000a1";
constexpr auto V2  = "7e100002-0000-0000-0000-0000000000a1";

wikore::RequestContext make_ctx() {
    return {
        .tenant    = {.company_id = CO},
        .principal = {.user_id = USR, .email = "test@test.com"},
        .span      = {.trace_id = "t", .span_id = "s"},
        .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };
}

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

// ---------------------------------------------------------------------------
// Helpers: wrap raw DB awaitable into Task so sync_wait accepts it.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Event-loop fixture: starts Drogon once for all integration tests.
// ---------------------------------------------------------------------------
struct DrogonLoop {
    std::thread       t;
    std::atomic<bool> ready{false};

    DrogonLoop() {
        if (!db_available()) return;
        wikore::Config cfg;
        cfg.database_url = std::getenv("DATABASE_URL");
        wikore::Db::init(cfg, /*pool_size=*/8);
        t = std::thread([this] {
            drogon::app()
                .setThreadNum(4)
                .registerBeginningAdvice([this] { ready.store(true); })
                .run();
        });
        while (!ready.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ~DrogonLoop() {
        if (t.joinable()) { drogon::app().quit(); t.join(); }
    }
};

static DrogonLoop s_loop;

// ---------------------------------------------------------------------------
// Seed test fixtures (idempotent - delete+reinsert per test run).
// ---------------------------------------------------------------------------
void seed_fixtures() {
    auto db = wikore::Db::get();
    exec_sync(db, "DELETE FROM companies WHERE id=$1", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id, name, slug) VALUES ($1,'TestCo','testco')", std::string(CO));
    exec_sync(db, "INSERT INTO org_units (id, company_id, name, type) VALUES ($1,$2,'Root','root')", std::string(OU), std::string(CO));
    exec_sync(db, "INSERT INTO org_unit_closure (company_id, ancestor_id, descendant_id, depth) VALUES ($1,$2,$2,0)", std::string(CO), std::string(OU));
    exec_sync(db, "INSERT INTO users (id, company_id, external_issuer, external_sub, email, display_name) VALUES ($1,$2,'iss','sub','u@t.com','U')", std::string(USR), std::string(CO));
    exec_sync(db, "INSERT INTO documents (id, company_id, owner_org_unit_id, filename) VALUES ($1,$2,$3,'t.pdf')", std::string(DOC), std::string(CO), std::string(OU));
    exec_sync(db, "INSERT INTO document_versions (id,company_id,document_id,version_no,source_hash,ingest_status,chunk_count,completed_at,lifecycle_status) VALUES ($1,$2,$3,1,'h1','done',3,now(),'draft')", std::string(V1), std::string(CO), std::string(DOC));
    exec_sync(db, "INSERT INTO document_versions (id,company_id,document_id,version_no,source_hash,ingest_status,chunk_count,completed_at,lifecycle_status) VALUES ($1,$2,$3,2,'h2','done',5,now(),'draft')", std::string(V2), std::string(CO), std::string(DOC));
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("PromoteDocumentVersion: version becomes active", "[integration][promote]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    seed_fixtures();

    wikore::application::PromoteDocumentVersionUseCase uc{wikore::Db::get()};
    auto result = drogon::sync_wait(
        uc.execute(make_ctx(), {.document_id = DOC, .version_id = V1}));

    REQUIRE(result.has_value());

    auto rows = exec_sync(wikore::Db::get(),
        "SELECT lifecycle_status, activated_at FROM document_versions WHERE id=$1",
        std::string(V1));
    REQUIRE(std::string(rows[0]["lifecycle_status"].c_str()) == "active");
    REQUIRE_FALSE(rows[0]["activated_at"].isNull());
}

TEST_CASE("PromoteDocumentVersion: second promotion deprecates the first", "[integration][promote]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    seed_fixtures();

    wikore::application::PromoteDocumentVersionUseCase uc{wikore::Db::get()};
    REQUIRE(drogon::sync_wait(uc.execute(make_ctx(), {.document_id = DOC, .version_id = V1})).has_value());
    REQUIRE(drogon::sync_wait(uc.execute(make_ctx(), {.document_id = DOC, .version_id = V2})).has_value());

    auto rows = exec_sync(wikore::Db::get(),
        "SELECT lifecycle_status FROM document_versions WHERE document_id=$1 ORDER BY version_no",
        std::string(DOC));
    REQUIRE(rows.size() == 2);
    REQUIRE(std::string(rows[0]["lifecycle_status"].c_str()) == "deprecated");
    REQUIRE(std::string(rows[1]["lifecycle_status"].c_str()) == "active");
}

TEST_CASE("PromoteDocumentVersion: audit_log and outbox written atomically", "[integration][promote]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    seed_fixtures();

    wikore::application::PromoteDocumentVersionUseCase uc{wikore::Db::get()};
    REQUIRE(drogon::sync_wait(uc.execute(make_ctx(), {.document_id = DOC, .version_id = V1})).has_value());

    auto audit = exec_sync(wikore::Db::get(),
        "SELECT COUNT(*) AS n FROM audit_log WHERE entity_id=$1 AND action='document.version.promoted'",
        std::string(V1));
    REQUIRE(std::stoi(audit[0]["n"].c_str()) == 1);

    auto outbox = exec_sync(wikore::Db::get(),
        "SELECT COUNT(*) AS n FROM outbox_events WHERE aggregate_id=$1 AND job_type='qdrant_resync_version_lifecycle'",
        std::string(V1));
    REQUIRE(std::stoi(outbox[0]["n"].c_str()) == 1);
}

TEST_CASE("PromoteDocumentVersion: archived version is terminal", "[integration][promote]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    seed_fixtures();

    exec_sync(wikore::Db::get(),
        "UPDATE document_versions SET lifecycle_status='archived' WHERE id=$1",
        std::string(V1));

    wikore::application::PromoteDocumentVersionUseCase uc{wikore::Db::get()};
    auto result = drogon::sync_wait(
        uc.execute(make_ctx(), {.document_id = DOC, .version_id = V1}));
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("UnitOfWork: rollback leaves no trace", "[integration][uow]") {
    if (!db_available()) SKIP("DATABASE_URL not set");
    seed_fixtures();

    auto db = wikore::Db::get();
    drogon::sync_wait([&db]() -> drogon::Task<void> {
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        co_await uow.exec(
            "INSERT INTO audit_log (company_id, actor_type, action, entity_type)"
            " VALUES ($1, 'service', 'test.rollback.sentinel', 'test')",
            std::string(CO));
        uow.rollback();
    }());

    auto rows = exec_sync(db,
        "SELECT COUNT(*) AS n FROM audit_log WHERE action='test.rollback.sentinel'");
    REQUIRE(std::stoi(rows[0]["n"].c_str()) == 0);
}

TEST_CASE("PG concurrency: 100 queries without pool exhaustion", "[integration][concurrency]") {
    if (!db_available()) SKIP("DATABASE_URL not set");

    auto db = wikore::Db::get();
    std::atomic<int> success{0};

    drogon::sync_wait([&db, &success]() -> drogon::Task<void> {
        std::vector<drogon::Task<void>> tasks;
        tasks.reserve(100);
        for (int i = 0; i < 100; ++i) {
            tasks.push_back([&db, &success, i]() -> drogon::Task<void> {
                co_await db->execSqlCoro("SELECT $1::int + 1", i);
                success.fetch_add(1, std::memory_order_relaxed);
            }());
        }
        for (auto& t : tasks) co_await t;
    }());

    REQUIRE(success.load() == 100);
}
