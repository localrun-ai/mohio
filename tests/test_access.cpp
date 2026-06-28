#include <catch2/catch_test_macros.hpp>
#include "wikore/access.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <algorithm>
#include <cstdlib>

// Integration tests for AccessService and fetch_access_scopes.
// Tests skip when DATABASE_URL is not set.
// DrogonLoop is owned by test_promote_version.cpp; this file shares it.

namespace {

// Fixed UUIDs -- "a05e" prefix for "access test"
constexpr auto CO   = "a05e0000-0000-0000-0000-000000000001";
constexpr auto UA   = "a05e0000-0000-0000-0000-000000000010"; // member of LEGAL, s_and_d
constexpr auto UB   = "a05e0000-0000-0000-0000-000000000011"; // member of HR_SUB, self_only
constexpr auto DOC1 = "a05e0000-0000-0000-0000-000000000020"; // owned by HR_SUB, no grants
constexpr auto DOC2 = "a05e0000-0000-0000-0000-000000000021"; // owned by HR_SUB, ou grant s_and_d on HR
constexpr auto DOC3 = "a05e0000-0000-0000-0000-000000000022"; // owned by HR_SUB, ou grant self_only on HR
constexpr auto DOC4 = "a05e0000-0000-0000-0000-000000000023"; // owned by HR_SUB, ou grant self_only on HR_SUB

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

// Org_unit IDs are resolved at seed time and stored here.
std::string g_root, g_legal, g_legal_sub, g_hr, g_hr_sub;

void seed_access_fixtures(drogon::orm::DbClientPtr db)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db,
        "INSERT INTO companies (id, name, slug) VALUES ($1::uuid, 'AccessTestCo', 'accesstest')",
        std::string(CO));

    auto root_rows = exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO));
    REQUIRE(!root_rows.empty());
    g_root = std::string(root_rows[0]["id"].c_str());

    auto insert_ou = [&](const std::string& name, const std::string& parent_id) {
        auto rows = exec_sync(db,
            "INSERT INTO org_units (company_id, parent_id, name, type) "
            "VALUES ($1::uuid, $2::uuid, $3, 'department') RETURNING id::text",
            std::string(CO), parent_id, name);
        REQUIRE(!rows.empty());
        return std::string(rows[0]["id"].c_str());
    };

    g_legal     = insert_ou("Legal",     g_root);
    g_hr        = insert_ou("HR",        g_root);
    g_legal_sub = insert_ou("LegalSub",  g_legal);
    g_hr_sub    = insert_ou("HRSub",     g_hr);

    auto insert_user = [&](const char* uid, const char* email) {
        exec_sync(db,
            "INSERT INTO users (id, company_id, external_issuer, external_sub, email, display_name) "
            "VALUES ($1::uuid, $2::uuid, 'iss', $3, $4, $4)",
            std::string(uid), std::string(CO),
            std::string("sub-") + uid, std::string(email));
    };
    insert_user(UA, "ua@test.com");
    insert_user(UB, "ub@test.com");

    // UA: member of LEGAL with applies_to='self_and_descendants', role admin
    exec_sync(db,
        "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'admin', 'self_and_descendants')",
        std::string(CO), std::string(UA), g_legal);

    // UB: member of HR_SUB with applies_to='self_only', role viewer
    exec_sync(db,
        "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'viewer', 'self_only')",
        std::string(CO), std::string(UB), g_hr_sub);

    // Documents all owned by HR_SUB
    auto ins_doc = [&](const char* doc_id) {
        exec_sync(db,
            "INSERT INTO documents (id, company_id, owner_org_unit_id, filename) "
            "VALUES ($1::uuid, $2::uuid, $3::uuid, 'test.md')",
            std::string(doc_id), std::string(CO), g_hr_sub);
    };
    ins_doc(DOC1);
    ins_doc(DOC2);
    ins_doc(DOC3);
    ins_doc(DOC4);

    // DOC2: org_unit grant -- principal=LEGAL, resource=HR, self_and_descendants
    exec_sync(db,
        "INSERT INTO resource_grants "
        "(company_id, resource_type, resource_id, resource_applies_to, "
        " principal_type, principal_id, principal_applies_to, permission, granted_by) "
        "VALUES ($1::uuid,'org_unit',$2::uuid,'self_and_descendants',"
        "        'org_unit',$3::uuid,'self_only','read',$4::uuid)",
        std::string(CO), g_hr, g_legal, std::string(UA));

    // DOC3: org_unit grant -- principal=LEGAL, resource=HR, self_only
    //   HR_SUB is NOT at depth=0 from HR, so this grant must NOT apply to DOC3.
    exec_sync(db,
        "INSERT INTO resource_grants "
        "(company_id, resource_type, resource_id, resource_applies_to, "
        " principal_type, principal_id, principal_applies_to, permission, granted_by) "
        "VALUES ($1::uuid,'org_unit',$2::uuid,'self_only',"
        "        'org_unit',$3::uuid,'self_only','read',$4::uuid)",
        std::string(CO), g_hr, g_legal, std::string(UA));

    // DOC4: org_unit grant -- principal=LEGAL, resource=HR_SUB, self_only
    //   HR_SUB IS at depth=0 from itself, so this grant applies to DOC4.
    exec_sync(db,
        "INSERT INTO resource_grants "
        "(company_id, resource_type, resource_id, resource_applies_to, "
        " principal_type, principal_id, principal_applies_to, permission, granted_by) "
        "VALUES ($1::uuid,'org_unit',$2::uuid,'self_only',"
        "        'org_unit',$3::uuid,'self_only','read',$4::uuid)",
        std::string(CO), g_hr_sub, g_legal, std::string(UA));
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::ranges::find(v, s) != v.end();
}

} // namespace

// ---------------------------------------------------------------------------
// Unit tests (no DB required)
// ---------------------------------------------------------------------------

TEST_CASE("Role: to_string / role_from_string round-trip", "[access]")
{
    using wikore::Role;
    CHECK(wikore::to_string(Role::viewer) == "viewer");
    CHECK(wikore::to_string(Role::editor) == "editor");
    CHECK(wikore::to_string(Role::admin)  == "admin");
    CHECK(wikore::role_from_string("viewer") == Role::viewer);
    CHECK(wikore::role_from_string("editor") == Role::editor);
    CHECK(wikore::role_from_string("admin")  == Role::admin);
    CHECK(!wikore::role_from_string("superuser").has_value());
}

// ---------------------------------------------------------------------------
// Integration tests
// ---------------------------------------------------------------------------

TEST_CASE("effective_read_orgs: self_and_descendants membership expands to subtree and ancestors",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_access_fixtures(db);

    // UA has admin in LEGAL (self_and_descendants).
    // Expected: LEGAL, LEGAL_SUB (subtree expansion) + ROOT (ancestor walk).
    wikore::AccessService svc{db};
    auto orgs = drogon::sync_wait(svc.effective_read_orgs(CO, UA, g_legal));

    CHECK(contains(orgs, g_legal));
    CHECK(contains(orgs, g_legal_sub));
    CHECK(contains(orgs, g_root));
    CHECK_FALSE(contains(orgs, g_hr));
    CHECK_FALSE(contains(orgs, g_hr_sub));
}

TEST_CASE("effective_read_orgs: self_only membership expands to ancestors only",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // UB has viewer in HR_SUB (self_only).
    // Expected: HR_SUB (self), HR, ROOT (ancestors). No descendant expansion.
    wikore::AccessService svc{db};
    auto orgs = drogon::sync_wait(svc.effective_read_orgs(CO, UB, g_hr_sub));

    CHECK(contains(orgs, g_hr_sub));
    CHECK(contains(orgs, g_hr));
    CHECK(contains(orgs, g_root));
    CHECK_FALSE(contains(orgs, g_legal));
    CHECK_FALSE(contains(orgs, g_legal_sub));
}

TEST_CASE("has_role: self_and_descendants membership propagates to descendants",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::AccessService svc{db};

    // UA: admin in LEGAL (self_and_descendants) -> admin on LEGAL and LEGAL_SUB
    auto r1 = drogon::sync_wait(svc.has_role(UA, g_legal,     wikore::Role::admin));
    auto r2 = drogon::sync_wait(svc.has_role(UA, g_legal_sub, wikore::Role::admin));
    auto r3 = drogon::sync_wait(svc.has_role(UA, g_legal,     wikore::Role::viewer)); // admin >= viewer
    auto r4 = drogon::sync_wait(svc.has_role(UA, g_hr,        wikore::Role::viewer)); // wrong branch
    CHECK(r1 == true);
    CHECK(r2 == true);
    CHECK(r3 == true);
    CHECK(r4 == false);
}

TEST_CASE("has_role: self_only membership does not propagate to descendants",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::AccessService svc{db};

    // UB: viewer in HR_SUB (self_only) -> viewer on HR_SUB only, NOT on HR
    auto r1 = drogon::sync_wait(svc.has_role(UB, g_hr_sub, wikore::Role::viewer));
    auto r2 = drogon::sync_wait(svc.has_role(UB, g_hr,     wikore::Role::viewer));
    CHECK(r1 == true);
    CHECK(r2 == false);
}

TEST_CASE("fetch_access_scopes: no grants -> owner only",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC1));
    REQUIRE(result.has_value());
    const auto& scopes = *result;

    // Only the owner (HR_SUB) should be present.
    CHECK(scopes.size() == 1);
    CHECK(contains(scopes, g_hr_sub));
}

TEST_CASE("fetch_access_scopes: org_unit grant self_and_descendants on ancestor includes principal",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // DOC2 has grant: principal=LEGAL, resource=HR, self_and_descendants.
    // HR_SUB is a descendant of HR -> grant covers DOC2.
    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC2));
    REQUIRE(result.has_value());
    const auto& scopes = *result;

    CHECK(contains(scopes, g_hr_sub));  // owner
    CHECK(contains(scopes, g_legal));   // grant principal
}

TEST_CASE("fetch_access_scopes: org_unit grant self_only on ancestor does not include principal",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // DOC3 has grant: principal=LEGAL, resource=HR, self_only.
    // HR_SUB is NOT at depth=0 from HR -> grant does NOT cover DOC3.
    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC3));
    REQUIRE(result.has_value());
    const auto& scopes = *result;

    CHECK(contains(scopes, g_hr_sub));       // owner
    CHECK_FALSE(contains(scopes, g_legal));  // grant does not apply
}

TEST_CASE("fetch_access_scopes: org_unit grant self_only on exact owner includes principal",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // DOC4 has grant: principal=LEGAL, resource=HR_SUB, self_only.
    // HR_SUB IS at depth=0 from HR_SUB -> grant covers DOC4.
    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC4));
    REQUIRE(result.has_value());
    const auto& scopes = *result;

    CHECK(contains(scopes, g_hr_sub));  // owner
    CHECK(contains(scopes, g_legal));   // grant applies (exact match)
}
