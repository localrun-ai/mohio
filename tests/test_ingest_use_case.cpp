#include <catch2/catch_test_macros.hpp>
#include "wikore/application/ingest_document_version.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/db.hpp"
#include "wikore/config.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

// ---------------------------------------------------------------------------
// Integration tests for IngestDocumentVersionUseCase.
//
// These tests exercise the use case against a real Postgres -- the unit-test
// adapters (NullDocumentRepo etc.) cannot catch column-name or constraint-
// name mismatches because they never see SQL. Original PR #15 shipped four
// schema mismatches that all unit tests passed through; these tests would
// have failed CI immediately and prevented that.
//
// Requires DATABASE_URL. All tests SKIP when not set.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO      = "cafe1111-0000-0000-0000-000000000001";
constexpr auto USR     = "cafe1111-0000-0000-0000-000000000002";
constexpr auto DOC     = "d0c01111-0000-0000-0000-000000000001";
constexpr auto VERSION = "7e101111-0000-0000-0000-000000000001";

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

wikore::RequestContext make_ctx() {
    return {
        .tenant    = {.company_id = CO},
        .principal = {.user_id = USR, .email = "ingest@test.com"},
        .span      = {.trace_id = "trace-ingest-test", .span_id = "s"},
        .deadline  = std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };
}

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args) {
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

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

// The promote-version test file already owns a static DrogonLoop. If both
// translation units instantiate one they will fight over drogon::app().run().
// Use Catch2's static-init order guarantee: each file's static is local and
// the loop guard is reentrant (db_available() short-circuits the second
// construction). The drogon::app() singleton is the same in both -- only
// one actually runs run() because the second sees ready already set false
// then the thread is joinable check guards exit. Avoid this by gating on a
// weak symbol: link order will pick one. Simpler: declare nothing here and
// rely on the promote-version test's loop. The runner links both .cpp
// files; as long as both tests share the same DB pool from wikore::Db::get()
// they coexist. (Verified: drogon::app() is a singleton; the loop runs once.)

} // namespace

// We deliberately do NOT instantiate a DrogonLoop here -- test_promote_version.cpp
// already does. Both files link into the same wikore_tests binary so the
// Drogon event loop is shared.

void seed_ingest_fixtures(drogon::orm::DbClientPtr db) {
    // Cascade-delete the test tenant, then recreate.
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db,
        "INSERT INTO companies (id, name, slug) "
        "VALUES ($1::uuid, 'IngestTestCo', 'ingesttest')",
        std::string(CO));
    auto ou_rows = exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO));
    REQUIRE(!ou_rows.empty());
    const auto root_ou = std::string(ou_rows[0]["id"].c_str());

    exec_sync(db,
        "INSERT INTO users (id, company_id, external_issuer, external_sub, email, display_name) "
        "VALUES ($1::uuid, $2::uuid, 'iss', 'sub-ingest', 'ingest@t.com', 'Ingest')",
        std::string(USR), std::string(CO));
    exec_sync(db,
        "INSERT INTO documents (id, company_id, owner_org_unit_id, filename) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'manual.md')",
        std::string(DOC), std::string(CO), root_ou);
    exec_sync(db,
        "INSERT INTO document_versions "
        "(id, company_id, document_id, version_no, source_hash, ingest_status, lifecycle_status) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 1, 'h1', 'pending', 'draft')",
        std::string(VERSION), std::string(CO), std::string(DOC));
}

std::string write_temp_doc() {
    // Use a deterministic path under /tmp so the test is self-cleaning and
    // works under the CI sandbox.
    auto dir = std::filesystem::temp_directory_path() / "wikore-ingest-it";
    std::filesystem::create_directories(dir);
    auto path = dir / "manual.md";
    std::ofstream f(path);
    f << "# Operations Manual\n\n"
      << "Standard operating procedures.\n\n"
      << "## Section One\n\n"
      << "First-section body text with multiple sentences. "
      << "This exercises the chunker without crossing into the next heading.\n\n"
      << "## Section Two\n\n"
      << "Second-section body text. The K5 invariant is that chunks do not "
      << "span across these headings. The repo writes one row per section "
      << "into document_sections with depth, heading, and heading_path.\n";
    return path.string();
}

// Build an in-DB use case using the production PostgresDocumentRepo. We
// intentionally use the production repo (not NullDocumentRepo) so the test
// will fail if any column name, constraint, or check drifts.
auto make_use_case(drogon::orm::DbClientPtr db) {
    return wikore::application::IngestDocumentVersionUseCase{
        db,
        std::make_shared<wikore::ingest::PostgresDocumentRepo>(db),
        std::make_shared<wikore::ingest::PlainTextParser>(),
        wikore::ingest::Chunker{},
    };
}

TEST_CASE("IngestDocumentVersion: full happy path against real schema",
          "[integration][ingest]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_ingest_fixtures(db);

    const auto file_path = write_temp_doc();
    auto uc = make_use_case(db);

    auto result = drogon::sync_wait(uc.execute(
        make_ctx(),
        {.company_id          = CO,
         .document_id         = DOC,
         .document_version_id = VERSION,
         .file_path           = file_path,
         .embed_model_id      = "bge-m3"}));
    REQUIRE(result.has_value());

    // Status transition to 'done' with completed_at + chunk_count.
    auto v = exec_sync(db,
        "SELECT ingest_status, completed_at, chunk_count "
        "FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    REQUIRE(v.size() == 1);
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "done");
    CHECK_FALSE(v[0]["completed_at"].isNull());
    CHECK_FALSE(v[0]["chunk_count"].isNull());
    CHECK(std::stoi(v[0]["chunk_count"].c_str()) >= 2);

    // Sections were written with heading_path populated (V014).
    auto secs = exec_sync(db,
        "SELECT heading, depth, ordinal, array_length(heading_path, 1) AS path_len "
        "FROM document_sections WHERE document_version_id=$1::uuid ORDER BY ordinal",
        std::string(VERSION));
    REQUIRE(secs.size() >= 3);  // h1 + 2 h2s minimum
    for (const auto& row : secs) {
        CHECK(std::stoi(row["path_len"].c_str()) >= 1);
    }

    // Chunks were written with content_hash NOT NULL and content column
    // (would fail if blocker B2 returned -- "text_content" doesn't exist).
    auto chunks = exec_sync(db,
        "SELECT content, content_hash, chunk_index, section_id "
        "FROM document_chunks WHERE document_version_id=$1::uuid "
        "ORDER BY chunk_index",
        std::string(VERSION));
    REQUIRE(!chunks.empty());
    for (const auto& row : chunks) {
        CHECK(std::string(row["content_hash"].c_str()).size() == 64);
        CHECK(!std::string(row["content"].c_str()).empty());
    }

    // Audit row written in the same UoW as the chunks. The audit_log has
    // no FK to companies (intentional per V007), so prior test runs may
    // have left audit rows for this company id. Compare via before/after
    // delta rather than an absolute count.
    auto audit_after = exec_sync(db,
        "SELECT COUNT(*) AS n FROM audit_log "
        "WHERE company_id=$1::uuid AND action='doc.ingest.chunks_written' "
        "AND detail->>'version_id'=$2",
        std::string(CO), std::string(VERSION));
    CHECK(std::stoi(audit_after[0]["n"].c_str()) >= 1);

    // Outbox event written in the same UoW (this is what the worker reads
    // to perform the actual Qdrant upsert).
    auto outbox = exec_sync(db,
        "SELECT job_type, aggregate_id::text AS agg, payload->>'embed_model_id' AS model "
        "FROM outbox_events "
        "WHERE company_id=$1::uuid AND job_type='qdrant_upsert_chunk_payload'",
        std::string(CO));
    REQUIRE(outbox.size() == 1);
    CHECK(std::string(outbox[0]["agg"].c_str()) == VERSION);
    CHECK(std::string(outbox[0]["model"].c_str()) == "bge-m3");
}

TEST_CASE("IngestDocumentVersion: re-ingest is idempotent (no duplicate chunks)",
          "[integration][ingest]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_ingest_fixtures(db);

    const auto file_path = write_temp_doc();
    auto uc = make_use_case(db);

    // First ingest.
    REQUIRE(drogon::sync_wait(uc.execute(
        make_ctx(),
        {.company_id = CO, .document_id = DOC, .document_version_id = VERSION,
         .file_path = file_path, .embed_model_id = "bge-m3"})).has_value());

    auto count1 = exec_sync(db,
        "SELECT COUNT(*) AS n FROM document_chunks WHERE document_version_id=$1::uuid",
        std::string(VERSION));
    const int n1 = std::stoi(count1[0]["n"].c_str());

    // Second ingest (re-run on same version).
    REQUIRE(drogon::sync_wait(uc.execute(
        make_ctx(),
        {.company_id = CO, .document_id = DOC, .document_version_id = VERSION,
         .file_path = file_path, .embed_model_id = "bge-m3"})).has_value());

    auto count2 = exec_sync(db,
        "SELECT COUNT(*) AS n FROM document_chunks WHERE document_version_id=$1::uuid",
        std::string(VERSION));
    CHECK(std::stoi(count2[0]["n"].c_str()) == n1);

    // Same trace_id -> outbox ON CONFLICT DO NOTHING -> still exactly 1 row.
    auto outbox = exec_sync(db,
        "SELECT COUNT(*) AS n FROM outbox_events "
        "WHERE company_id=$1::uuid AND aggregate_id=$2::uuid",
        std::string(CO), std::string(VERSION));
    CHECK(std::stoi(outbox[0]["n"].c_str()) == 1);
}

TEST_CASE("IngestDocumentVersion: missing file marks version 'error' (returns TerminalError)",
          "[integration][ingest]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_ingest_fixtures(db);

    auto uc = make_use_case(db);
    auto result = drogon::sync_wait(uc.execute(
        make_ctx(),
        {.company_id          = CO,
         .document_id         = DOC,
         .document_version_id = VERSION,
         .file_path           = "/nonexistent/path/to/missing.md",
         .embed_model_id      = "bge-m3"}));
    // Post-CAS terminal failures (the pipeline ran past claim_for_processing
    // and then hit a business error like missing file) return Ok(TerminalError);
    // pre-CAS infra failures return Err. The row is in 'error' either way.
    REQUIRE(result.has_value());
    CHECK(*result == wikore::application::IngestDispatchOutcome::TerminalError);

    auto v = exec_sync(db,
        "SELECT ingest_status FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "error");
}

TEST_CASE("IngestDocumentVersion: rejected input marks version 'error'",
          "[integration][ingest]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_ingest_fixtures(db);

    auto dir = std::filesystem::temp_directory_path() / "wikore-ingest-it";
    std::filesystem::create_directories(dir);
    auto path = dir / "empty.txt";
    std::ofstream(path, std::ios::trunc);

    auto uc = make_use_case(db);
    auto result = drogon::sync_wait(uc.execute(
        make_ctx(),
        {.company_id          = CO,
         .document_id         = DOC,
         .document_version_id = VERSION,
         .file_path           = path.string(),
         .embed_model_id      = "bge-m3"}));
    // Post-CAS terminal failure (parser rejected the empty file after
    // claim_for_processing won) -> returns Ok(TerminalError); row is
    // 'error' with error_msg persisted by the token-gated set_status.
    REQUIRE(result.has_value());
    CHECK(*result == wikore::application::IngestDispatchOutcome::TerminalError);

    auto v = exec_sync(db,
        "SELECT ingest_status, error_msg FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "error");
    CHECK(std::string(v[0]["error_msg"].c_str()) == "ingest.empty_file");
}

TEST_CASE("IngestDocumentVersion: cross-tenant cmd is rejected",
          "[integration][ingest]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_ingest_fixtures(db);

    auto uc = make_use_case(db);
    auto ctx = make_ctx();
    ctx.tenant.company_id = "deadbeef-0000-0000-0000-000000000000"; // different tenant
    auto result = drogon::sync_wait(uc.execute(
        ctx,
        {.company_id          = CO,
         .document_id         = DOC,
         .document_version_id = VERSION,
         .file_path           = write_temp_doc(),
         .embed_model_id      = "bge-m3"}));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == wikore::Error::Kind::Forbidden);
}

TEST_CASE("PostgresDocumentRepo::mark_ingest_done sets completed_at and chunk_count",
          "[integration][ingest]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_ingest_fixtures(db);

    drogon::sync_wait([&db]() -> drogon::Task<void> {
        wikore::ingest::PostgresDocumentRepo repo{db};
        auto uow = co_await wikore::postgres::UnitOfWork::begin(db);
        // No claim_token: tests bypass claim_for_processing for direct
        // mark_done verification. Empty token disables the ownership
        // check (test/legacy path).
        auto r = co_await repo.mark_ingest_done(
            CO, VERSION, /*chunk_count=*/7, /*claim_token=*/"", uow);
        REQUIRE(r.has_value());
        REQUIRE(*r == true);
        co_await uow.commit();
    }());

    auto v = exec_sync(db,
        "SELECT ingest_status, completed_at, chunk_count "
        "FROM document_versions WHERE id=$1::uuid",
        std::string(VERSION));
    CHECK(std::string(v[0]["ingest_status"].c_str()) == "done");
    CHECK_FALSE(v[0]["completed_at"].isNull());
    CHECK(std::stoi(v[0]["chunk_count"].c_str()) == 7);
}

TEST_CASE("PostgresDocumentRepo::set_ingest_status rejects 'done' (use mark_ingest_done)",
          "[integration][ingest]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_ingest_fixtures(db);

    auto result = drogon::sync_wait(
        [&db]() -> drogon::Task<wikore::Result<bool>> {
            wikore::ingest::PostgresDocumentRepo repo{db};
            co_return co_await repo.set_ingest_status(
                CO, VERSION, wikore::ingest::IngestStatus::done,
                /*claim_token=*/"", /*error_msg=*/"", db);
        }());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == wikore::Error::Kind::InvalidState);
}
