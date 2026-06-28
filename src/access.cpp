#include "wikore/access.hpp"
#include <drogon/orm/DbClient.h>
#include <spdlog/spdlog.h>

namespace wikore {

std::string_view to_string(Role r)
{
    switch (r) {
    case Role::viewer: return "viewer";
    case Role::editor: return "editor";
    case Role::admin:  return "admin";
    }
    return "viewer";
}

std::optional<Role> role_from_string(std::string_view s)
{
    if (s == "viewer") return Role::viewer;
    if (s == "editor") return Role::editor;
    if (s == "admin")  return Role::admin;
    return std::nullopt;
}

AccessService::AccessService(drogon::orm::DbClientPtr db) : _db(std::move(db)) {}

// ---------------------------------------------------------------------------
// effective_read_orgs
//
// Returns the set of org_unit IDs to use as the Qdrant MatchAny filter for
// a user. The set is intersected with each chunk's access_scope_ids.
//
// Set-algebra contract (see also fetch_access_scopes in document_repo.cpp):
//
//   access_scope_ids stores GRANT ROOTS: owner org_unit + explicit grant
//   principals. For a user to match grant root G in MatchAny, G must be in
//   this set.
//
//   Two-join expansion:
//     sub: expand memberships with applies_to='self_and_descendants' into the
//          full descendant subtree. A user in a PARENT org_unit also "acts as"
//          all child org_units (matching owner-entry access_scope_ids for docs
//          owned by descendants).
//     anc: walk UP from each effective org_unit to include all ancestors. A
//          grant root G stored for a principal_applies_to='self_and_descendants'
//          grant is reachable by members of G's descendants.
//
// Example: doc-level grant with principal=Legal, principal_applies_to='self_and_descendants'.
//   access_scope_ids = {Legal}.
//   User in Legal-Subteam: anc gives {Legal-Subteam, Legal, ...}.
//   MatchAny fires on Legal -> access granted.
// ---------------------------------------------------------------------------

drogon::Task<std::vector<std::string>>
AccessService::effective_read_orgs(std::string_view company_id,
                                   std::string_view user_id,
                                   std::string_view /*org_unit_id*/)
{
    constexpr auto kSql = R"(
        SELECT DISTINCT anc.ancestor_id::text AS org_unit_id
        FROM   memberships m
        JOIN   org_unit_closure sub
            ON sub.company_id  = $1::uuid
           AND sub.ancestor_id = m.org_unit_id
           AND (m.applies_to = 'self_and_descendants' OR sub.depth = 0)
        JOIN   org_unit_closure anc
            ON anc.company_id    = $1::uuid
           AND anc.descendant_id = sub.descendant_id
        WHERE  m.company_id = $1::uuid
          AND  m.user_id    = $2::uuid
          AND  (m.expires_at IS NULL OR m.expires_at > now())
    )";

    try {
        auto rows = co_await _db->execSqlCoro(kSql,
            std::string(company_id), std::string(user_id));
        std::vector<std::string> result;
        result.reserve(rows.size());
        for (const auto& row : rows)
            result.push_back(row["org_unit_id"].as<std::string>());
        co_return result;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[access] effective_read_orgs failed user={}: {}",
                      user_id, ex.base().what());
        co_return std::vector<std::string>{};  // fail-closed
    }
}

// ---------------------------------------------------------------------------
// has_role
//
// Returns true if the user holds at least `required` on org_unit_id.
// Checks direct membership AND ancestor memberships with
// applies_to='self_and_descendants' (which extends role to all descendants).
// ---------------------------------------------------------------------------

drogon::Task<bool>
AccessService::has_role(std::string_view user_id,
                        std::string_view org_unit_id,
                        Role             required)
{
    constexpr auto kSql = R"(
        SELECT EXISTS (
            SELECT 1
            FROM   memberships m
            JOIN   org_unit_closure c
                ON c.company_id    = m.company_id
               AND c.ancestor_id   = m.org_unit_id
               AND c.descendant_id = $2::uuid
            WHERE  m.user_id   = $1::uuid
              AND  (m.expires_at IS NULL OR m.expires_at > now())
              AND  (m.applies_to = 'self_and_descendants' OR c.depth = 0)
              AND  m.role = ANY(
                       CASE $3::text
                           WHEN 'viewer' THEN ARRAY['viewer','editor','admin']
                           WHEN 'editor' THEN ARRAY['editor','admin']
                           WHEN 'admin'  THEN ARRAY['admin']
                           ELSE ARRAY[]::text[]
                       END
                   )
        ) AS has_access
    )";

    try {
        auto rows = co_await _db->execSqlCoro(kSql,
            std::string(user_id),
            std::string(org_unit_id),
            std::string(to_string(required)));
        if (rows.empty()) co_return false;
        co_return rows[0]["has_access"].as<bool>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[access] has_role failed user={} org={}: {}",
                      user_id, org_unit_id, ex.base().what());
        co_return false;  // fail-closed
    }
}

// ---------------------------------------------------------------------------
// Cache invalidation (stub - Redis cache not yet wired to this service)
// ---------------------------------------------------------------------------

drogon::Task<void>
AccessService::invalidate_cache(std::string_view, std::string_view, std::string_view)
{
    co_return;
}

// ---------------------------------------------------------------------------
// Membership and grant CRUD (stubs - API layer, Iteration 2)
// ---------------------------------------------------------------------------

drogon::Task<void>
AccessService::add_member(std::string_view, std::string_view, std::string_view,
                          std::string_view, Role, bool, std::string_view)
{
    co_return;
}

drogon::Task<void>
AccessService::remove_member(std::string_view, std::string_view, std::string_view)
{
    co_return;
}

drogon::Task<void>
AccessService::change_role(std::string_view, std::string_view, std::string_view, Role)
{
    co_return;
}

drogon::Task<void>
AccessService::grant_resource(std::string_view, std::string_view, std::string_view,
                              std::string_view, std::string_view, std::string_view,
                              bool, std::string_view)
{
    co_return;
}

drogon::Task<void>
AccessService::revoke_resource(std::string_view, std::string_view,
                               std::string_view, std::string_view)
{
    co_return;
}

} // namespace wikore
