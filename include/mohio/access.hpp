#pragma once
#include <string>
#include <vector>
#include <optional>
#include <drogon/drogon.h>

namespace mohio {

// Roles that can appear in memberships and api_keys.
enum class Role { viewer, editor, admin };

std::string_view to_string(Role r);
std::optional<Role> role_from_string(std::string_view s);

// Resolves the set of org_unit IDs a principal can access when querying
// a given org_unit (the "access_scope_ids" used in Qdrant payload filters).
//
// Algorithm:
//   1. Walk up the org_unit_closure ancestors; collect org_units where the
//      principal has membership with applies_to='self_and_descendants'.
//   2. Collect org_units in the subtree where the principal has direct
//      membership (any applies_to value).
//   3. Add org_units reached via resource_grants to orgs the principal
//      already belongs to.
//
// Result is cached in Redis at lr:eff:{company_id}:{user_id}:{org_unit_id}.
// Must be invalidated on any membership or resource_grant change.
class AccessService {
public:
    explicit AccessService(drogon::orm::DbClientPtr db);

    // Returns org_unit_ids the principal can READ when scoped to org_unit_id.
    // This is the exact set passed as the Qdrant 'access_scope_ids' filter.
    drogon::Task<std::vector<std::string>>
    effective_read_orgs(std::string_view company_id,
                        std::string_view user_id,
                        std::string_view org_unit_id);

    // Returns true if the principal has at least `required` on the exact org_unit.
    drogon::Task<bool>
    has_role(std::string_view user_id,
             std::string_view org_unit_id,
             Role             required);

    // Membership CRUD (invalidates Redis cache and enqueues Qdrant resync).
    drogon::Task<void>
    add_member(std::string_view company_id,
               std::string_view org_unit_id,
               std::string_view principal_type,   // "user" or "group"
               std::string_view principal_id,
               Role             role,
               bool             self_and_descendants,
               std::string_view granted_by);

    drogon::Task<void>
    remove_member(std::string_view org_unit_id,
                  std::string_view principal_type,
                  std::string_view principal_id);

    drogon::Task<void>
    change_role(std::string_view org_unit_id,
                std::string_view principal_type,
                std::string_view principal_id,
                Role             new_role);

    // Resource grants.
    drogon::Task<void>
    grant_resource(std::string_view company_id,
                   std::string_view resource_type,   // "org_unit"|"document"|"wiki_page"
                   std::string_view resource_id,
                   std::string_view principal_type,  // "user"|"group"|"org_unit"
                   std::string_view principal_id,
                   std::string_view permission,      // "read"|"write"|"admin"
                   bool             self_and_descendants,
                   std::string_view granted_by);

    drogon::Task<void>
    revoke_resource(std::string_view resource_type,
                    std::string_view resource_id,
                    std::string_view principal_type,
                    std::string_view principal_id);

private:
    drogon::orm::DbClientPtr _db;

    drogon::Task<void> invalidate_cache(std::string_view company_id,
                                        std::string_view user_id,
                                        std::string_view org_unit_id);
};

} // namespace mohio
