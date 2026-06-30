#include <catch2/catch_test_macros.hpp>
#include "wikore/access_resolver.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

// Integration tests for PostgresAccessResolver (Iteration 2 read-path resolver).
// Skip when DATABASE_URL is unset. The scope-set correctness is covered broadly
// by the G1 property test and test_access; here we focus on the resolver's
// contract: the scope set, the (acl_epoch, scope_epoch) stamp, and that the
// stamp reflects the V032 bump triggers (the basis for cache validation).

namespace {

constexpr auto CO = "a5e50000-0000-0000-0000-0000000000a1"; // "access resolver co"

bool db_available()    { return std::getenv("DATABASE_URL") != nullptr; }
bool redis_available() { return std::getenv("REDIS_URL")    != nullptr; }

// Inner resolver that counts calls and returns a canned scope, so we can
// observe whether the cache served a hit (inner not called) or re-resolved.
struct SpyResolver : wikore::AccessResolverPort {
    mutable int          calls = 0;
    wikore::AccessScope  canned;
    drogon::Task<wikore::Result<wikore::AccessScope>>
    resolve(std::string_view, std::string_view, std::string_view) const override {
        ++calls;
        co_return canned;
    }
};

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

struct Fixture {
    std::string root, ou, user;
};

Fixture seed(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db, "INSERT INTO companies (id,name,slug) VALUES ($1::uuid,'AR','ar')",
              std::string(CO));
    Fixture f;
    f.root = std::string(exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO))[0]["id"].c_str());
    f.ou = std::string(exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'department','eng','Eng') RETURNING id",
        std::string(CO), f.root)[0]["id"].c_str());
    f.user = std::string(exec_sync(db,
        "INSERT INTO users (company_id,external_sub,email) "
        "VALUES ($1::uuid,'subAR','ar@test') RETURNING id",
        std::string(CO))[0]["id"].c_str());
    return f;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("AccessResolver: resolves scope and stamps epochs", "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    // Member of eng (self_and_descendants).
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants')",
        std::string(CO), f.user, f.ou);

    auto r = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r.has_value());
    CHECK(contains(r->org_unit_ids, f.ou));
    CHECK(r->access_epoch >= 1);  // companies.acl_epoch defaults to 1 (V032)
    CHECK(r->scope_epoch  >= 1);  // users.scope_epoch defaults to 1, +1 per membership
    CHECK_FALSE(r->org_unit_ids.empty());
}

TEST_CASE("AccessResolver: no memberships yields empty scope but valid epochs (not an error)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);                 // user created, no memberships
    wikore::PostgresAccessResolver resolver(db);

    auto r = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r.has_value());
    CHECK(r->org_unit_ids.empty());     // fail-closed: no scope
    CHECK(r->access_epoch >= 1);        // epochs still stamped (cache can validate)
    CHECK(r->scope_epoch  >= 1);
}

TEST_CASE("AccessResolver: unknown principal is NotFound", "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    auto r = drogon::sync_wait(resolver.resolve(
        CO, "deadbeef-0000-0000-0000-000000000000", f.root));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == wikore::Error::Kind::NotFound);
}

TEST_CASE("AccessResolver: scope_epoch stamp advances after a membership change (V032 trigger)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    auto r0 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r0.has_value());
    const auto e0 = r0->scope_epoch;

    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only')",
        std::string(CO), f.user, f.ou);

    auto r1 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r1.has_value());
    INFO("scope_epoch e0=" << e0 << " e1=" << r1->scope_epoch);
    CHECK(r1->scope_epoch > e0);   // a stale cache stamped with e0 is now detectable
}

TEST_CASE("AccessResolver: acl_epoch stamp advances after an org structural change (V032 trigger)",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    auto r0 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r0.has_value());
    const auto a0 = r0->access_epoch;

    // Org-unit create is a structural change -> companies.acl_epoch bumps (V032).
    exec_sync(db,
        "INSERT INTO org_units (company_id,parent_id,type,slug,name) "
        "VALUES ($1::uuid,$2::uuid,'team','t2','T2')",
        std::string(CO), f.root);

    auto r1 = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    REQUIRE(r1.has_value());
    INFO("acl_epoch a0=" << a0 << " a1=" << r1->access_epoch);
    CHECK(r1->access_epoch > a0);
}

TEST_CASE("Resolver: cache_until defaults to ~5min and clamps to earliest membership expiry",
          "[integration][access_resolver]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    using namespace std::chrono;
    auto db = wikore::Db::get();
    auto f  = seed(db);
    wikore::PostgresAccessResolver resolver(db);

    // Non-expiring membership -> default ~5 minutes.
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only')",
        std::string(CO), f.user, f.ou);
    auto r1   = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    auto now1 = system_clock::now();
    REQUIRE(r1.has_value());
    CHECK(r1->cache_until >  now1 + minutes(4));
    CHECK(r1->cache_until <= now1 + minutes(5) + seconds(5));

    // Add a membership expiring in 30s -> cache_until clamps well under 5min.
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to,expires_at) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_and_descendants', now()+interval '30 seconds')",
        std::string(CO), f.user, f.root);
    auto r2   = drogon::sync_wait(resolver.resolve(CO, f.user, f.root));
    auto now2 = system_clock::now();
    REQUIRE(r2.has_value());
    CHECK(r2->cache_until <= now2 + seconds(60));
    CHECK(r2->cache_until >  now2);
}

TEST_CASE("CachedAccessResolver: epoch-valid hit is served from cache; an epoch bump re-resolves",
          "[integration][access_resolver]")
{
    if (!db_available() || !redis_available())
        SKIP("DATABASE_URL or REDIS_URL not set");
    using namespace std::chrono;
    auto db = wikore::Db::get();
    auto f  = seed(db);

    // Stamp the canned scope with the LIVE epochs so the cache validation passes.
    auto ep = exec_sync(db,
        "SELECT c.acl_epoch, u.scope_epoch FROM companies c "
        "JOIN users u ON u.company_id=c.id WHERE c.id=$1::uuid AND u.id=$2::uuid",
        std::string(CO), f.user);
    auto spy = std::make_shared<SpyResolver>();
    spy->canned.access_epoch = ep[0]["acl_epoch"].as<int>();
    spy->canned.scope_epoch  = ep[0]["scope_epoch"].as<int>();
    spy->canned.cache_until  = system_clock::now() + minutes(5);
    spy->canned.org_unit_ids = {f.ou};

    wikore::CachedAccessResolver cached(spy, db);

    auto r1 = drogon::sync_wait(cached.resolve(CO, f.user, f.root));
    REQUIRE(r1.has_value());
    CHECK(spy->calls == 1);                       // miss -> inner resolved
    CHECK(contains(r1->org_unit_ids, f.ou));

    auto r2 = drogon::sync_wait(cached.resolve(CO, f.user, f.root));
    REQUIRE(r2.has_value());
    CHECK(spy->calls == 1);                       // HIT -> inner NOT called again
    CHECK(contains(r2->org_unit_ids, f.ou));

    // Bump users.scope_epoch via a membership change (V032 trigger): the stamp
    // is now stale and the next resolve must fall through to inner.
    exec_sync(db,
        "INSERT INTO memberships (company_id,user_id,org_unit_id,role,applies_to) "
        "VALUES ($1::uuid,$2::uuid,$3::uuid,'viewer','self_only')",
        std::string(CO), f.user, f.ou);

    auto r3 = drogon::sync_wait(cached.resolve(CO, f.user, f.root));
    REQUIRE(r3.has_value());
    INFO("spy calls after epoch bump=" << spy->calls);
    CHECK(spy->calls == 2);                       // stale epoch -> re-resolve
}
