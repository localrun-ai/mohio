#pragma once
#include <string>
#include <vector>
#include <drogon/drogon.h>

namespace mohio {

enum class Role { reader, writer, admin };

std::string_view to_string(Role r);
std::optional<Role> role_from_string(std::string_view s);

// Resolves the set of org IDs a user can access when querying a given org.
//
// Algorithm:
//   1. Walk up org's ancestor chain; collect orgs where user has membership.
//   2. Walk down org's subtree; include children the user has direct membership in.
//   3. Add any orgs explicitly granted to orgs the user belongs to (access_grants).
//
// Result is cached in Redis at lr:eff:{user_id}:{org_id} for 5 minutes.
// Invalidated on any membership or grant change affecting the user.
class AccessService {
public:
    AccessService(drogon::orm::DbClientPtr db,
                  nosql::RedisClientPtr    redis);

    // Returns org IDs the user can READ when scoped to org_id.
    // These become the Qdrant collection filter for RAG queries.
    drogon::Task<std::vector<std::string>>
    effective_read_orgs(std::string_view user_id, std::string_view org_id);

    // Returns true if the user has at least the given role on the exact org.
    // No upward inheritance for writer/admin.
    drogon::Task<bool>
    has_role(std::string_view user_id,
             std::string_view org_id,
             Role             required);

    // Membership CRUD (invalidates Redis cache on change).
    drogon::Task<void>
    add_member(std::string_view org_id,
               std::string_view user_id,
               Role             role,
               std::string_view granted_by);

    drogon::Task<void>
    remove_member(std::string_view org_id, std::string_view user_id);

    drogon::Task<void>
    change_role(std::string_view org_id,
                std::string_view user_id,
                Role             new_role);

    // Cross-org access grants.
    drogon::Task<void>
    grant_access(std::string_view grantee_org_id,
                 std::string_view target_org_id,
                 Role             role,
                 std::string_view granted_by);

    drogon::Task<void>
    revoke_access(std::string_view grantee_org_id,
                  std::string_view target_org_id);

private:
    drogon::orm::DbClientPtr  _db;
    nosql::RedisClientPtr     _redis;

    drogon::Task<void> invalidate_cache(std::string_view user_id,
                                        std::string_view org_id);
};

} // namespace mohio
