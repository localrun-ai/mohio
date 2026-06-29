#pragma once
// Internal header - not part of the public adapter API.
// Exposed so unit tests can verify the constraint extraction logic against the
// production regex without duplicating it, and verify the constraint-name
// coverage of error_mapper.cpp's k_constraint_map.
#include <regex>
#include <string>
#include <string_view>

namespace wikore::postgres::detail {

inline std::string extract_constraint(const std::string& msg) {
    static const std::regex re(R"re(constraint "([^"]+)")re");
    std::smatch m;
    if (std::regex_search(msg, m, re))
        return m[1].str();
    return {};
}

// True if `name` has a typed Error mapping in error_mapper.cpp's
// k_constraint_map. Used by the pg_constraint introspection test to assert
// the map keeps up with the migrations.
bool is_constraint_mapped(std::string_view name);

} // namespace wikore::postgres::detail
