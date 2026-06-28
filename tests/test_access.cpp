#include <catch2/catch_test_macros.hpp>
#include "wikore/access.hpp"
#include "wikore/ingest/document_repo.hpp"
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
constexpr auto CO  = "a05e0000-0000-0000-0000-000000000001";
constexpr auto UA  = "a05e0000-0000-0000-0000-000000000010"; // direct LEGAL member (s_and_d)
constexpr auto UB  = "a05e0000-0000-0000-0000-000000000011"; // direct HR_SUB member (self_only)
constexpr auto UC  = "a05e0000-0000-0000-0000-000000000012"; // member of both LEGAL and HR (s_and_d)
constexpr auto UG  = "a05e0000-0000-0000-0000-000000000013"; // member via group
constexpr auto GRP = "a05e0000-0000-0000-0000-000000000090"; // group with LEGAL membership

// DOC1: owned by HR_SUB, no grants
constexpr auto DOC1 = "a05e0000-0000-0000-0000-000000000020";
// DOC2: owned by HR_SUB, ou grant (principal=LEGAL s_only, resource=HR s_and_d)
constexpr auto DOC2 = "a05e0000-0000-0000-0000-000000000021";
// DOC3: owned by HR_SUB, ou grant (principal=LEGAL s_only, resource=HR s_only) -- must NOT match
constexpr auto DOC3 = "a05e0000-0000-0000-0000-000000000022";
// DOC4: owned by HR_SUB, ou grant (principal=LEGAL s_only, resource=HR_SUB s_only) -- exact match
constexpr auto DOC4 = "a05e0000-0000-0000-0000-000000000023";
// DOC5: owned by HR_SUB, doc grant (principal=LEGAL s_and_d) -- principal subtree expanded
constexpr auto DOC5 = "a05e0000-0000-0000-0000-000000000024";

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

    auto insert_ou = [&](const std::string& name, const std::string& slug,
                         const std::string& parent_id) {
        auto rows = exec_sync(db,
            "INSERT INTO org_units (company_id, parent_id, name, slug, type) "
            "VALUES ($1::uuid, $2::uuid, $3, $4, 'department') RETURNING id::text",
            std::string(CO), parent_id, name, slug);
        REQUIRE(!rows.empty());
        return std::string(rows[0]["id"].c_str());
    };

    g_legal     = insert_ou("Legal",    "legal",     g_root);
    g_hr        = insert_ou("HR",       "hr",        g_root);
    g_legal_sub = insert_ou("LegalSub", "legalsub",  g_legal);
    g_hr_sub    = insert_ou("HRSub",    "hrsub",     g_hr);

    auto insert_user = [&](const char* uid, const char* email) {
        exec_sync(db,
            "INSERT INTO users (id, company_id, external_issuer, external_sub, email, display_name) "
            "VALUES ($1::uuid, $2::uuid, 'iss', $3, $4, $4)",
            std::string(uid), std::string(CO),
            std::string("sub-") + uid, std::string(email));
    };
    insert_user(UA, "ua@test.com");
    insert_user(UB, "ub@test.com");
    insert_user(UC, "uc@test.com");
    insert_user(UG, "ug@test.com");

    // Group with LEGAL membership
    exec_sync(db,
        "INSERT INTO groups (id, company_id, name, slug) VALUES ($1::uuid, $2::uuid, 'LegalGroup', 'legalgroup')",
        std::string(GRP), std::string(CO));
    exec_sync(db,
        "INSERT INTO memberships (company_id, group_id, org_unit_id, role, applies_to) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'editor', 'self_and_descendants')",
        std::string(CO), std::string(GRP), g_legal);
    exec_sync(db,
        "INSERT INTO group_members (company_id, group_id, user_id) VALUES ($1::uuid, $2::uuid, $3::uuid)",
        std::string(CO), std::string(GRP), std::string(UG));

    // UA: LEGAL, self_and_descendants, admin
    exec_sync(db,
        "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'admin', 'self_and_descendants')",
        std::string(CO), std::string(UA), g_legal);

    // UB: HR_SUB, self_only, viewer
    exec_sync(db,
        "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'viewer', 'self_only')",
        std::string(CO), std::string(UB), g_hr_sub);

    // UC: LEGAL (s_and_d) and HR (s_and_d) -- multi-branch user
    exec_sync(db,
        "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'viewer', 'self_and_descendants')",
        std::string(CO), std::string(UC), g_legal);
    exec_sync(db,
        "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
        "VALUES ($1::uuid, $2::uuid, $3::uuid, 'viewer', 'self_and_descendants')",
        std::string(CO), std::string(UC), g_hr);

    auto ins_doc = [&](const char* doc_id) {
        exec_sync(db,
            "INSERT INTO documents (id, company_id, owner_org_unit_id, filename) "
            "VALUES ($1::uuid, $2::uuid, $3::uuid, 'test.md')",
            std::string(doc_id), std::string(CO), g_hr_sub);
    };
    ins_doc(DOC1); ins_doc(DOC2); ins_doc(DOC3); ins_doc(DOC4); ins_doc(DOC5);

    auto grant = [&](const std::string& resource_id, const char* res_applies,
                     const std::string& principal_id, const char* pri_applies,
                     const char* rtype = "org_unit") {
        exec_sync(db,
            "INSERT INTO resource_grants "
            "(company_id, resource_type, resource_id, resource_applies_to, "
            " principal_type, principal_id, principal_applies_to, permission, granted_by) "
            "VALUES ($1::uuid,$2,$3::uuid,$4,'org_unit',$5::uuid,$6,'read',$7::uuid)",
            std::string(CO), std::string(rtype),
            resource_id, std::string(res_applies),
            principal_id, std::string(pri_applies), std::string(UA));
    };

    // DOC2: ou grant, resource=HR s_and_d, principal=LEGAL s_only
    grant(g_hr, "self_and_descendants", g_legal, "self_only");
    // DOC3: ou grant, resource=HR self_only -- HR_SUB is depth>0, must NOT apply
    grant(g_hr, "self_only", g_legal, "self_only");
    // DOC4: ou grant, resource=HR_SUB self_only -- exact match, must apply
    grant(g_hr_sub, "self_only", g_legal, "self_only");
    // DOC5: doc-level grant, principal=LEGAL s_and_d -- LEGAL_SUB also stored
    exec_sync(db,
        "INSERT INTO resource_grants "
        "(company_id, resource_type, resource_id, resource_applies_to, "
        " principal_type, principal_id, principal_applies_to, permission, granted_by) "
        "VALUES ($1::uuid,'document',$2::uuid,'self_only','org_unit',$3::uuid,'self_and_descendants','read',$4::uuid)",
        std::string(CO), std::string(DOC5), g_legal, std::string(UA));
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::ranges::find(v, s) != v.end();
}

} // namespace

// ---------------------------------------------------------------------------
// Unit tests
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
// effective_read_orgs integration tests
// ---------------------------------------------------------------------------

TEST_CASE("effective_read_orgs: self_and_descendants expands subtree, scoped to Legal",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    seed_access_fixtures(db);

    // UA: admin in LEGAL (self_and_descendants), scoped to LEGAL.
    // Expected: LEGAL and LEGAL_SUB (subtree). ROOT is NOT included (no ancestor walk).
    wikore::AccessService svc{db};
    auto orgs = drogon::sync_wait(svc.effective_read_orgs(CO, UA, g_legal));

    CHECK(contains(orgs, g_legal));
    CHECK(contains(orgs, g_legal_sub));
    CHECK_FALSE(contains(orgs, g_root));   // no ancestor walk
    CHECK_FALSE(contains(orgs, g_hr));
    CHECK_FALSE(contains(orgs, g_hr_sub));
}

TEST_CASE("effective_read_orgs: self_only membership, scoped to HR_SUB",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // UB: viewer in HR_SUB (self_only), scoped to HR_SUB.
    // Expected: HR_SUB only. No subtree expansion, no ancestor walk.
    wikore::AccessService svc{db};
    auto orgs = drogon::sync_wait(svc.effective_read_orgs(CO, UB, g_hr_sub));

    CHECK(contains(orgs, g_hr_sub));
    CHECK_FALSE(contains(orgs, g_hr));
    CHECK_FALSE(contains(orgs, g_root));
    CHECK_FALSE(contains(orgs, g_legal));
    CHECK_FALSE(contains(orgs, g_legal_sub));
}

TEST_CASE("effective_read_orgs: self_only scoped to HR includes HR_SUB",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // UB is a member of HR_SUB (self_only). When browsing HR context,
    // HR_SUB is a descendant of HR so it passes the scope filter.
    wikore::AccessService svc{db};
    auto orgs = drogon::sync_wait(svc.effective_read_orgs(CO, UB, g_hr));

    CHECK(contains(orgs, g_hr_sub));  // in HR's scope
    CHECK_FALSE(contains(orgs, g_hr)); // not a member of HR itself
}

TEST_CASE("effective_read_orgs: multi-branch user scoped to Legal excludes HR branch",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // UC: member of both LEGAL and HR (self_and_descendants).
    // Scoped to LEGAL: should only include Legal subtree.
    wikore::AccessService svc{db};
    auto orgs = drogon::sync_wait(svc.effective_read_orgs(CO, UC, g_legal));

    CHECK(contains(orgs, g_legal));
    CHECK(contains(orgs, g_legal_sub));
    CHECK_FALSE(contains(orgs, g_hr));      // HR branch excluded by scope
    CHECK_FALSE(contains(orgs, g_hr_sub));
}

TEST_CASE("effective_read_orgs: group-derived membership works",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // UG is a member of GRP which has LEGAL (self_and_descendants) membership.
    // Scoped to LEGAL: should include LEGAL and LEGAL_SUB.
    wikore::AccessService svc{db};
    auto orgs = drogon::sync_wait(svc.effective_read_orgs(CO, UG, g_legal));

    CHECK(contains(orgs, g_legal));
    CHECK(contains(orgs, g_legal_sub));
    CHECK_FALSE(contains(orgs, g_hr));
}

// ---------------------------------------------------------------------------
// has_role integration tests
// ---------------------------------------------------------------------------

TEST_CASE("has_role: self_and_descendants propagates to descendants",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::AccessService svc{db};
    auto r1 = drogon::sync_wait(svc.has_role(UA, g_legal,     wikore::Role::admin));
    auto r2 = drogon::sync_wait(svc.has_role(UA, g_legal_sub, wikore::Role::admin));
    auto r3 = drogon::sync_wait(svc.has_role(UA, g_legal,     wikore::Role::viewer)); // admin >= viewer
    auto r4 = drogon::sync_wait(svc.has_role(UA, g_hr,        wikore::Role::viewer)); // wrong branch
    CHECK(r1 == true);
    CHECK(r2 == true);
    CHECK(r3 == true);
    CHECK(r4 == false);
}

TEST_CASE("has_role: self_only does not propagate to descendants",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::AccessService svc{db};
    auto r1 = drogon::sync_wait(svc.has_role(UB, g_hr_sub, wikore::Role::viewer));
    auto r2 = drogon::sync_wait(svc.has_role(UB, g_hr,     wikore::Role::viewer));
    CHECK(r1 == true);
    CHECK(r2 == false);
}

TEST_CASE("has_role: group-derived membership grants role",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::AccessService svc{db};
    // UG is in GRP which has editor in LEGAL (self_and_descendants)
    auto r1 = drogon::sync_wait(svc.has_role(UG, g_legal,     wikore::Role::editor));
    auto r2 = drogon::sync_wait(svc.has_role(UG, g_legal_sub, wikore::Role::editor));
    auto r3 = drogon::sync_wait(svc.has_role(UG, g_legal,     wikore::Role::admin)); // editor < admin
    CHECK(r1 == true);
    CHECK(r2 == true);
    CHECK(r3 == false);
}

// ---------------------------------------------------------------------------
// fetch_access_scopes integration tests
// ---------------------------------------------------------------------------

TEST_CASE("fetch_access_scopes: no grants -> owner only",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC1));
    REQUIRE(result.has_value());

    CHECK(result->size() == 1);
    CHECK(contains(*result, g_hr_sub));
}

TEST_CASE("fetch_access_scopes: ou grant self_and_descendants on resource ancestor includes principal",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // DOC2: grant resource=HR (s_and_d), principal=LEGAL (s_only).
    // HR_SUB is a descendant of HR -> grant applies; LEGAL in access_scope_ids.
    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC2));
    REQUIRE(result.has_value());

    CHECK(contains(*result, g_hr_sub));  // owner
    CHECK(contains(*result, g_legal));   // grant principal (self_only -> not expanded)
    CHECK_FALSE(contains(*result, g_legal_sub)); // not expanded (self_only)
}

TEST_CASE("fetch_access_scopes: ou grant self_only on resource ancestor does not apply",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // DOC3: grant resource=HR (self_only), principal=LEGAL (s_only).
    // HR_SUB is at depth>0 from HR -> self_only resource does not cover DOC3.
    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC3));
    REQUIRE(result.has_value());

    CHECK(contains(*result, g_hr_sub));
    CHECK_FALSE(contains(*result, g_legal));
}

TEST_CASE("fetch_access_scopes: ou grant self_only on exact owner applies",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // DOC4: grant resource=HR_SUB (self_only), principal=LEGAL (s_only).
    // HR_SUB IS at depth=0 from HR_SUB -> applies; LEGAL in access_scope_ids.
    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC4));
    REQUIRE(result.has_value());

    CHECK(contains(*result, g_hr_sub));
    CHECK(contains(*result, g_legal));
}

TEST_CASE("fetch_access_scopes: doc grant self_and_descendants expands principal subtree",
          "[integration][access]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();

    // DOC5: doc-level grant to LEGAL with principal_applies_to=self_and_descendants.
    // access_scope_ids must include LEGAL_SUB so a LegalSub member can retrieve it
    // without any ancestor walk at query time.
    wikore::ingest::PostgresDocumentRepo repo{db};
    auto result = drogon::sync_wait(repo.fetch_access_scopes(CO, DOC5));
    REQUIRE(result.has_value());

    CHECK(contains(*result, g_hr_sub));      // owner
    CHECK(contains(*result, g_legal));       // grant principal
    CHECK(contains(*result, g_legal_sub));   // expanded descendant
}
