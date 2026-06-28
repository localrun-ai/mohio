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
// Returns the set of org_unit IDs to pass as the Qdrant MatchAny filter.
// This set is intersected with each chunk's access_scope_ids at query time.
//
// SET-ALGEBRA CONTRACT (see fetch_access_scopes in document_repo.cpp):
//   access_scope_ids stores a RESOLVED set (ingest-time expansion):
//     - self_only grants store only the grant principal.
//     - self_and_descendants grants store principal + all its descendants.
//   Therefore effective_read_orgs does NOT need an ancestor walk.
//   It returns only the org_units the user is EFFECTIVELY A MEMBER OF:
//     - direct memberships (any applies_to)
//     - membership org_unit + all descendants when applies_to='self_and_descendants'
//     - same for group-derived memberships
//   Result is then constrained to descendants-or-self of org_unit_id so that
//   a multi-membership user scoped to one branch cannot retrieve content
//   accessible only via a sibling branch membership.
//
// Example: doc grant principal=Legal, principal_applies_to='self_and_descendants'.
//   access_scope_ids at ingest = {Legal, LegalSub}.
//   User in LegalSub: effective_read_orgs(scope=Legal) = {Legal, LegalSub}.
//   MatchAny: LegalSub in both -> access granted.
//   User in HR (sibling, not in Legal scope): effective_read_orgs(scope=Legal) = {}.
//   MatchAny: empty -> no results (fail-closed).
// ---------------------------------------------------------------------------

drogon::Task<std::vector<std::string>>
AccessService::effective_read_orgs(std::string_view company_id,
                                   std::string_view user_id,
                                   std::string_view org_unit_id)
{
    constexpr auto kSql = R"(
        SELECT DISTINCT sub.descendant_id::text AS org_unit_id
        FROM (
            -- Direct user memberships
            SELECT m.org_unit_id, m.applies_to
            FROM   memberships m
            WHERE  m.company_id = $1::uuid
              AND  m.user_id    = $2::uuid
              AND  (m.expires_at IS NULL OR m.expires_at > now())
            UNION
            -- Group-derived memberships
            SELECT m.org_unit_id, m.applies_to
            FROM   group_members gm
            JOIN   memberships m
                ON m.company_id = gm.company_id
               AND m.group_id   = gm.group_id
            WHERE  gm.company_id = $1::uuid
              AND  gm.user_id    = $2::uuid
              AND  (m.expires_at IS NULL OR m.expires_at > now())
        ) base
        -- Expand self_and_descendants into subtree; self_only stays at depth 0
        JOIN org_unit_closure sub
            ON sub.company_id  = $1::uuid
           AND sub.ancestor_id = base.org_unit_id
           AND (base.applies_to = 'self_and_descendants' OR sub.depth = 0)
        -- Constrain to the requested org_unit_id scope (descendants-or-self)
        JOIN org_unit_closure scope
            ON scope.company_id    = $1::uuid
           AND scope.ancestor_id   = $3::uuid
           AND scope.descendant_id = sub.descendant_id
    )";

    try {
        auto rows = co_await _db->execSqlCoro(kSql,
            std::string(company_id),
            std::string(user_id),
            std::string(org_unit_id));
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
// Checks direct and group-derived memberships, both direct (any applies_to)
// and ancestral memberships with applies_to='self_and_descendants'.
// ---------------------------------------------------------------------------

drogon::Task<bool>
AccessService::has_role(std::string_view user_id,
                        std::string_view org_unit_id,
                        Role             required)
{
    constexpr auto kSql = R"(
        SELECT EXISTS (
            SELECT 1
            FROM (
                SELECT m.org_unit_id, m.applies_to, m.role, m.company_id
                FROM   memberships m
                WHERE  m.user_id = $1::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
                UNION ALL
                SELECT m.org_unit_id, m.applies_to, m.role, m.company_id
                FROM   group_members gm
                JOIN   memberships m
                    ON m.company_id = gm.company_id
                   AND m.group_id   = gm.group_id
                WHERE  gm.user_id = $1::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
            ) base
            JOIN org_unit_closure c
                ON c.company_id    = base.company_id
               AND c.ancestor_id   = base.org_unit_id
               AND c.descendant_id = $2::uuid
            WHERE (base.applies_to = 'self_and_descendants' OR c.depth = 0)
              AND base.role = ANY(
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
//
// TODO(resync): each mutation that changes memberships, grants, org-unit
// ownership, or the org tree must enqueue an outbox event to re-run
// fetch_access_scopes over all affected document_chunks and push updated
// Qdrant payloads. Until this resync worker is implemented, revocations and
// grant additions do not take effect until documents are re-ingested.
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
