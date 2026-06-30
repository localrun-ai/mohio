#include "wikore/access_resolver.hpp"
#include <drogon/drogon.h>

namespace wikore {

drogon::Task<Result<AccessScope>>
PostgresAccessResolver::resolve(std::string_view company_id,
                                std::string_view user_id,
                                std::string_view scope_org_unit_id) const
{
    // SINGLE statement: the scope set AND (acl_epoch, scope_epoch) in one Read
    // Committed snapshot. The scope CTE mirrors AccessService::
    // effective_read_orgs (proven + tested). epochs is LEFT JOINed so the
    // epochs are returned even when the scope is empty; org_unit_id is NULL on
    // that single epoch-only row. Zero rows means the (company, user) pair does
    // not exist -> NotFound (tenant isolation: the join requires u.company_id = $1).
    constexpr auto kSql = R"(
        WITH scope AS (
            SELECT DISTINCT sub.descendant_id::text AS org_unit_id
            FROM (
                SELECT m.org_unit_id, m.applies_to
                FROM   memberships m
                WHERE  m.company_id = $1::uuid AND m.user_id = $2::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
                UNION
                SELECT m.org_unit_id, m.applies_to
                FROM   group_members gm
                JOIN   memberships m
                    ON m.company_id = gm.company_id AND m.group_id = gm.group_id
                WHERE  gm.company_id = $1::uuid AND gm.user_id = $2::uuid
                  AND  (m.expires_at IS NULL OR m.expires_at > now())
            ) base
            JOIN org_unit_closure sub
                ON sub.company_id  = $1::uuid
               AND sub.ancestor_id = base.org_unit_id
               AND (base.applies_to = 'self_and_descendants' OR sub.depth = 0)
            JOIN org_unit_closure scope
                ON scope.company_id    = $1::uuid
               AND scope.ancestor_id   = $3::uuid
               AND scope.descendant_id = sub.descendant_id
        ),
        epochs AS (
            SELECT c.acl_epoch, u.scope_epoch
            FROM   companies c
            JOIN   users u ON u.company_id = c.id
            WHERE  c.id = $1::uuid AND u.id = $2::uuid
        )
        SELECT e.acl_epoch    AS acl_epoch,
               e.scope_epoch  AS scope_epoch,
               s.org_unit_id  AS org_unit_id
        FROM   epochs e
        LEFT JOIN scope s ON true
    )";

    try {
        auto rows = co_await db_->execSqlCoro(
            kSql, std::string(company_id), std::string(user_id),
            std::string(scope_org_unit_id));

        if (rows.empty())
            co_return std::unexpected(Error::not_found(
                "access_resolver: principal not found in tenant"));

        AccessScope scope;
        scope.access_epoch = rows[0]["acl_epoch"].as<int>();
        scope.scope_epoch  = rows[0]["scope_epoch"].as<int>();
        scope.org_unit_ids.reserve(rows.size());
        for (const auto& r : rows)
            if (!r["org_unit_id"].isNull())
                scope.org_unit_ids.push_back(r["org_unit_id"].as<std::string>());
        co_return scope;
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(Error::database_error(
            std::string("access_resolver: ") + ex.base().what()));
    }
}

} // namespace wikore
