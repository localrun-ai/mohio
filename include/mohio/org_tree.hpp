#pragma once
#include <string>
#include <vector>
#include <optional>
#include <drogon/drogon.h>

namespace mohio {

// Matches the CHECK constraint in V001: org_units.type
enum class OrgUnitType {
    subsidiary, division, department, team, project
};

std::string_view to_string(OrgUnitType t);
std::optional<OrgUnitType> org_unit_type_from_string(std::string_view s);

struct OrgUnit {
    std::string                id;
    std::string                company_id;
    std::optional<std::string> parent_id;
    OrgUnitType                type;
    std::string                slug;
    std::string                name;
    std::string                description;
    std::vector<OrgUnit>       children;   // populated by get_subtree()
};

struct Company {
    std::string id;
    std::string name;
    std::string slug;
};

// All methods are Drogon async coroutines using the PostgreSQL client
// from Db::get(). org_unit_closure is maintained by DB trigger, so tree
// traversal is a single indexed lookup rather than a recursive CTE.
class OrgTreeService {
public:
    explicit OrgTreeService(drogon::orm::DbClientPtr db);

    // Companies
    drogon::Task<Company> create_company(std::string name, std::string slug);
    drogon::Task<std::optional<Company>> get_company(std::string_view id);

    // Org units
    drogon::Task<OrgUnit> create(std::string_view company_id,
                                 std::optional<std::string> parent_id,
                                 OrgUnitType type,
                                 std::string slug,
                                 std::string name,
                                 std::string description = {});

    drogon::Task<std::optional<OrgUnit>> get(std::string_view id);

    drogon::Task<OrgUnit> update(std::string_view id,
                                 std::optional<std::string> name,
                                 std::optional<std::string> description);

    drogon::Task<void> remove(std::string_view id);

    // Immediate children (one level, from org_units.parent_id index).
    drogon::Task<std::vector<OrgUnit>> children(std::string_view org_unit_id);

    // Full subtree rooted at org_unit_id (via org_unit_closure).
    drogon::Task<OrgUnit> subtree(std::string_view org_unit_id);

    // All ancestor IDs from org_unit_id up to the root (nearest first),
    // resolved via org_unit_closure.ancestor_id lookup.
    drogon::Task<std::vector<std::string>> ancestors(std::string_view org_unit_id);

    // All descendant IDs (for access scope resolution).
    drogon::Task<std::vector<std::string>> descendants(std::string_view org_unit_id);

private:
    drogon::orm::DbClientPtr _db;
};

} // namespace mohio
