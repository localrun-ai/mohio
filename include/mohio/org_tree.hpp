#pragma once
#include <string>
#include <vector>
#include <optional>
#include <drogon/drogon.h>

namespace mohio {

enum class OrgType { company, subsidiary, division, department, team };

std::string_view to_string(OrgType t);
std::optional<OrgType> org_type_from_string(std::string_view s);

struct OrgNode {
    std::string              id;
    std::optional<std::string> parent_id;
    OrgType                  type;
    std::string              slug;
    std::string              name;
    std::string              description;
    std::vector<OrgNode>     children;   // populated by get_tree()
};

// All methods are async coroutines using Drogon's PostgreSQL client.
class OrgTreeService {
public:
    explicit OrgTreeService(drogon::orm::DbClientPtr db);

    // Create a new org node. parent_id = nullopt -> root (company only).
    drogon::Task<OrgNode> create(std::optional<std::string> parent_id,
                                 OrgType type,
                                 std::string slug,
                                 std::string name,
                                 std::string description = {});

    drogon::Task<std::optional<OrgNode>> get(std::string_view id);

    drogon::Task<OrgNode> update(std::string_view id,
                                 std::optional<std::string> name,
                                 std::optional<std::string> description);

    drogon::Task<void> remove(std::string_view id);

    // Immediate children of org_id (one level).
    drogon::Task<std::vector<OrgNode>> children(std::string_view org_id);

    // Full subtree rooted at org_id (recursive).
    drogon::Task<OrgNode> subtree(std::string_view org_id);

    // All ancestor IDs from org_id up to the root (nearest first).
    drogon::Task<std::vector<std::string>> ancestors(std::string_view org_id);

private:
    drogon::orm::DbClientPtr _db;
};

} // namespace mohio
