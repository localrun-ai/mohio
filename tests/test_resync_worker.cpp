#include <catch2/catch_test_macros.hpp>
#include "wikore/scheduler/resync_worker.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ResyncWorker integration tests.
//
// Exercises the qdrant_resync_chunk_acl path: a resource_grant change fires the
// V032 trigger (bumps documents.acl_version, enqueues a resync outbox event);
// the worker recomputes the live scope, refreshes the denormalized column, and
// set_payloads the Qdrant points (a NullVectorStore here) WITHOUT re-embedding,
// then CAS-advances documents.qdrant_synced_version.
//
// Skip without DATABASE_URL. The DrogonLoop fixture in test_promote_version.cpp
// serves these tests (single loop per process).
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO       = "c0000000-0000-0000-0000-0000000000c1";
constexpr auto DOC      = "d0c50000-0000-0000-0000-000000000001";
constexpr auto VER      = "7e150000-0000-0000-0000-000000000001";
constexpr auto CH       = "c8150000-0000-0000-0000-000000000001";
constexpr auto MODEL_ID = "11ed5000-0000-0000-0000-000000000001";
constexpr auto POINT_ID = "b0150000-0000-0000-0000-000000000001";
// Second embedding model / collection, for the multi-collection routing test.
constexpr auto MODEL_ID2 = "11ed5000-0000-0000-0000-000000000002";
constexpr auto POINT_ID2 = "b0150000-0000-0000-0000-000000000002";
constexpr auto COLL1     = "wikore_resync_test";
constexpr auto COLL2     = "wikore_resync_test2";
constexpr int  DIM       = 4;

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

std::string g_root, g_team;

// Seed a company with a document owned by root, an active version + one chunk,
// a vector-bookkeeping row pointing at POINT_ID, and a stale prefilter scope
// ({root} only, missing the team we are about to grant). Returns with g_root /
// g_team populated.
void seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'Resync','resync')",
              std::string(CO));
    g_root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
    g_team = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','rteam','rteam') RETURNING id",
        std::string(CO), g_root)[0]["id"].c_str());

    // Distinct qdrant_collection from other suites (embedding_models.name AND
    // .qdrant_collection are both globally UNIQUE; the registry has no
    // company_id, so it is shared across every test's tenant).
    exec_sync(db,
        "INSERT INTO embedding_models (id,name,qdrant_collection,dimension) "
        "VALUES ($1::uuid,'null-resync','wikore_resync_test',4) "
        "ON CONFLICT (name) DO NOTHING",
        std::string(MODEL_ID));

    exec_sync(db,
        "INSERT INTO documents (id,company_id,owner_org_unit_id,filename) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'r.txt')",
        std::string(DOC), std::string(CO), g_root);
    exec_sync(db,
        "INSERT INTO document_versions (id,company_id,document_id,version_no,source_hash,"
        "ingest_status,completed_at,activated_at,chunk_count,lifecycle_status,sensitivity_label) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,1,'h','done',now(),now(),1,'active','internal')",
        std::string(VER), std::string(CO), std::string(DOC));
    // Stale denormalized scope: {root} only.
    exec_sync(db,
        "INSERT INTO document_chunks (id,company_id,document_version_id,chunk_index,content,"
        "content_hash,qdrant_prefilter_scope_ids) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,0,'body','hh',ARRAY[$4::uuid])",
        std::string(CH), std::string(CO), std::string(VER), g_root);
    exec_sync(db,
        "INSERT INTO document_chunk_vectors (company_id,chunk_id,embedding_model_id,qdrant_point_id) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid)",
        std::string(CO), std::string(CH), std::string(MODEL_ID), std::string(POINT_ID));
}

// A NullVectorStore seeded with one point carrying a STALE payload: scope
// {root}, acl_version 1, schema v2. Resync must overwrite these.
std::shared_ptr<wikore::rag::NullVectorStore> store_with(const std::string& point_id)
{
    auto store = std::make_shared<wikore::rag::NullVectorStore>();
    wikore::rag::ChunkPayload p;
    p.company_id          = CO;
    p.document_id         = DOC;
    p.document_version_id = VER;
    p.owner_org_unit_id   = g_root;
    p.chunk_id            = CH;
    p.access_scope_ids    = {g_root};
    p.sensitivity_label   = "internal";
    p.lifecycle_status    = "active";
    p.acl_version         = 1;
    p.payload_schema_version = 2;
    drogon::sync_wait(store->upsert(std::vector<wikore::rag::UpsertPoint>{
        {.id = point_id, .vector = wikore::rag::Embedding(DIM, 0.1f), .payload = std::move(p)}}));
    return store;
}
std::shared_ptr<wikore::rag::NullVectorStore> seeded_store() { return store_with(POINT_ID); }

// Grant read on the document to the team org_unit. Fires the V032 trigger
// (bumps documents.acl_version and enqueues resync:{DOC}:v).
void grant_team_read(drogon::orm::DbClientPtr db)
{
    exec_sync(db,
        "INSERT INTO resource_grants (company_id,resource_type,resource_id,"
        "principal_type,principal_id,permission) "
        "VALUES ($1::uuid,'document',$2::uuid,'org_unit',$3::uuid,'read')",
        std::string(CO), std::string(DOC), g_team);
}

// A collection resolver that returns the same store for any collection name
// (the NullVectorStore holds points keyed only by point id, so it stands in for
// every collection in these tests).
wikore::scheduler::ResyncWorker make_worker(
    drogon::orm::DbClientPtr db,
    std::shared_ptr<wikore::rag::VectorStorePort> store,
    std::atomic<bool>& stop)
{
    return wikore::scheduler::ResyncWorker(
        db,
        [store](const std::string&) { return store; },
        std::make_shared<wikore::ingest::PostgresDocumentRepo>(db),
        [&] { return stop.load(); },
        wikore::scheduler::ResyncWorker::Options{});
}

} // namespace

TEST_CASE("ResyncWorker: a grant change refreshes the payload scope without re-embedding",
          "[integration][resync]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    auto store = seeded_store();

    // Grant read on the document to the team org_unit. The V032 trigger bumps
    // documents.acl_version to 2 and enqueues resync:{DOC}:2.
    exec_sync(db,
        "INSERT INTO resource_grants (company_id,resource_type,resource_id,"
        "principal_type,principal_id,permission) "
        "VALUES ($1::uuid,'document',$2::uuid,'org_unit',$3::uuid,'read')",
        std::string(CO), std::string(DOC), g_team);

    // Sanity: acl_version bumped, an event was enqueued, synced_version lags.
    auto d0 = exec_sync(db,
        "SELECT acl_version, qdrant_synced_version FROM documents WHERE id=$1::uuid",
        std::string(DOC));
    REQUIRE(std::stoll(d0[0]["acl_version"].c_str()) == 2);
    REQUIRE(std::stoll(d0[0]["qdrant_synced_version"].c_str()) == 0);

    std::atomic<bool> stop{false};
    auto worker = make_worker(db, store, stop);
    // drain_once is a GLOBAL drainer (job_type only, no tenant filter), so in
    // the full suite it may also process other tenants' pending resync events.
    // Assert on THIS document's outcome, not the aggregate counters.
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);

    // Outbox event completed.
    auto ev = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done FROM outbox_events "
        "WHERE company_id=$1::uuid AND aggregate_id=$2::uuid "
        "  AND job_type='qdrant_resync_chunk_acl'",
        std::string(CO), std::string(DOC));
    REQUIRE(ev.size() == 1);
    CHECK(std::string(ev[0]["done"].c_str()) == "t");

    // CAS advanced synced_version to acl_version.
    auto d1 = exec_sync(db,
        "SELECT qdrant_synced_version FROM documents WHERE id=$1::uuid", std::string(DOC));
    CHECK(std::stoll(d1[0]["qdrant_synced_version"].c_str()) == 2);

    // Denormalized column now includes the team scope.
    auto col = exec_sync(db,
        "SELECT $2::uuid = ANY(qdrant_prefilter_scope_ids) AS has_team "
        "FROM document_chunks WHERE id=$1::uuid", std::string(CH), g_team);
    CHECK(std::string(col[0]["has_team"].c_str()) == "t");

    // Qdrant payload was rewritten in place (same point, no new points).
    CHECK(store->point_count() == 1);
    const auto* pl = store->payload_for(POINT_ID);
    REQUIRE(pl != nullptr);
    CHECK(std::ranges::find(pl->access_scope_ids, g_team) != pl->access_scope_ids.end());
    CHECK(std::ranges::find(pl->access_scope_ids, g_root) != pl->access_scope_ids.end());
    CHECK(pl->acl_version == 2);
    CHECK(pl->payload_schema_version == wikore::rag::ChunkPayload::kSchemaVersion);
}

TEST_CASE("ResyncWorker: a superseded event (acl_version < current) is dropped, not written",
          "[integration][resync]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    auto store = seeded_store();

    // Move the document to acl_version = 5, already fully synced.
    exec_sync(db,
        "UPDATE documents SET acl_version=5, qdrant_synced_version=5 WHERE id=$1::uuid",
        std::string(DOC));

    // Enqueue a STALE event (acl_version = 3) as if a slow/out-of-order bump.
    exec_sync(db,
        "INSERT INTO outbox_events (company_id,aggregate_id,job_type,payload,idempotency_key) "
        "VALUES ($1::uuid,$2::uuid,'qdrant_resync_chunk_acl',"
        "        jsonb_build_object('document_id',$2::text,'acl_version',3),"
        "        'resync:manual:stale')",
        std::string(CO), std::string(DOC));

    std::atomic<bool> stop{false};
    auto worker = make_worker(db, store, stop);
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);
    CHECK(worker.events_superseded() >= 1);

    // synced_version must NOT be dragged backwards to 3.
    auto d = exec_sync(db,
        "SELECT qdrant_synced_version FROM documents WHERE id=$1::uuid", std::string(DOC));
    CHECK(std::stoll(d[0]["qdrant_synced_version"].c_str()) == 5);

    // Payload untouched (still the stale seed: scope {root}, acl_version 1).
    const auto* pl = store->payload_for(POINT_ID);
    REQUIRE(pl != nullptr);
    CHECK(pl->acl_version == 1);
    CHECK(std::ranges::find(pl->access_scope_ids, g_team) == pl->access_scope_ids.end());

    // Event still marked completed (drained), not left to retry forever.
    auto ev = exec_sync(db,
        "SELECT completed_at IS NOT NULL AS done FROM outbox_events "
        "WHERE idempotency_key='resync:manual:stale'");
    CHECK(std::string(ev[0]["done"].c_str()) == "t");
}

TEST_CASE("ResyncWorker: no vectors yet still advances synced_version (no stale points)",
          "[integration][resync]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    // Drop the vector bookkeeping so there are no points to patch.
    exec_sync(db, "DELETE FROM document_chunk_vectors WHERE chunk_id=$1::uuid", std::string(CH));
    auto store = std::make_shared<wikore::rag::NullVectorStore>();

    exec_sync(db,
        "INSERT INTO resource_grants (company_id,resource_type,resource_id,"
        "principal_type,principal_id,permission) "
        "VALUES ($1::uuid,'document',$2::uuid,'org_unit',$3::uuid,'read')",
        std::string(CO), std::string(DOC), g_team);

    std::atomic<bool> stop{false};
    auto worker = make_worker(db, store, stop);
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);

    auto d = exec_sync(db,
        "SELECT acl_version, qdrant_synced_version FROM documents WHERE id=$1::uuid",
        std::string(DOC));
    CHECK(std::stoll(d[0]["qdrant_synced_version"].c_str())
          == std::stoll(d[0]["acl_version"].c_str()));
    CHECK(store->point_count() == 0);
}

TEST_CASE("ResyncWorker: patches EVERY collection the document has points in",
          "[integration][resync]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);   // model 1 (COLL1) + POINT_ID already seeded

    // A second embedding model in its OWN collection, with a second point for
    // the same chunk.
    exec_sync(db,
        "INSERT INTO embedding_models (id,name,qdrant_collection,dimension) "
        "VALUES ($1::uuid,'null-resync-2',$2,4) ON CONFLICT (name) DO NOTHING",
        std::string(MODEL_ID2), std::string(COLL2));
    exec_sync(db,
        "INSERT INTO document_chunk_vectors (company_id,chunk_id,embedding_model_id,qdrant_point_id) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,$4::uuid)",
        std::string(CO), std::string(CH), std::string(MODEL_ID2), std::string(POINT_ID2));

    // Distinct store per collection; the resolver routes by name.
    auto store1 = store_with(POINT_ID);
    auto store2 = store_with(POINT_ID2);
    auto resolve = [&](const std::string& c) -> std::shared_ptr<wikore::rag::VectorStorePort> {
        if (c == COLL1) return store1;
        if (c == COLL2) return store2;
        return nullptr;
    };
    grant_team_read(db);

    std::atomic<bool> stop{false};
    wikore::scheduler::ResyncWorker worker(
        db, resolve, std::make_shared<wikore::ingest::PostgresDocumentRepo>(db),
        [&] { return stop.load(); }, wikore::scheduler::ResyncWorker::Options{});
    int processed = drogon::sync_wait(worker.drain_once());
    CHECK(processed >= 1);

    // BOTH collections' points were refreshed (not just the first).
    for (auto* pl : {store1->payload_for(POINT_ID), store2->payload_for(POINT_ID2)}) {
        REQUIRE(pl != nullptr);
        CHECK(std::ranges::find(pl->access_scope_ids, g_team) != pl->access_scope_ids.end());
        CHECK(pl->acl_version == 2);
    }
    auto d = exec_sync(db,
        "SELECT qdrant_synced_version FROM documents WHERE id=$1::uuid", std::string(DOC));
    CHECK(std::stoll(d[0]["qdrant_synced_version"].c_str()) == 2);
}

TEST_CASE("ResyncWorker: an unservable collection fails the event (no silent advance)",
          "[integration][resync]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed(db);
    grant_team_read(db);

    // Resolver serves NO collection: the document's points live in COLL1, which
    // this worker cannot reach. Advancing synced_version would be a silent lie.
    std::atomic<bool> stop{false};
    wikore::scheduler::ResyncWorker worker(
        db,
        [](const std::string&) -> std::shared_ptr<wikore::rag::VectorStorePort> { return nullptr; },
        std::make_shared<wikore::ingest::PostgresDocumentRepo>(db),
        [&] { return stop.load(); }, wikore::scheduler::ResyncWorker::Options{});
    drogon::sync_wait(worker.drain_once());

    // synced_version NOT advanced; the event is left uncompleted to retry.
    auto d = exec_sync(db,
        "SELECT qdrant_synced_version FROM documents WHERE id=$1::uuid", std::string(DOC));
    CHECK(std::stoll(d[0]["qdrant_synced_version"].c_str()) == 0);
    auto ev = exec_sync(db,
        "SELECT completed_at IS NULL AS pending, attempt_count FROM outbox_events "
        "WHERE company_id=$1::uuid AND aggregate_id=$2::uuid "
        "  AND job_type='qdrant_resync_chunk_acl'",
        std::string(CO), std::string(DOC));
    REQUIRE(ev.size() == 1);
    CHECK(std::string(ev[0]["pending"].c_str()) == "t");
    CHECK(std::stoi(ev[0]["attempt_count"].c_str()) >= 1);
}
