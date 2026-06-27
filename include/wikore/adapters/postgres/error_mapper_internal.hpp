#pragma once
// Internal header - not part of the public adapter API.
// Exposed so unit tests can verify the constraint extraction logic against the
// production regex without duplicating it.
#include <regex>
#include <string>

namespace wikore::postgres::detail {

inline std::string extract_constraint(const std::string& msg) {
    static const std::regex re(R"re(constraint "([^"]+)")re");
    std::smatch m;
    if (std::regex_search(msg, m, re))
        return m[1].str();
    return {};
}

} // namespace wikore::postgres::detail
