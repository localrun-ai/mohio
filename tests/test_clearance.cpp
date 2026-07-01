#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/clearance.hpp"
#include <algorithm>

using wikore::rag::clearance_for;
using wikore::rag::allowed_labels_for;
using wikore::rag::SensitivityLevel;

namespace {
bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}

TEST_CASE("clearance: authenticated member gets up to confidential, never restricted",
          "[clearance]")
{
    wikore::Principal member{.user_id = "u", .email = "m@x", .is_admin = false};
    CHECK(clearance_for(member) == SensitivityLevel::confidential);

    const auto labels = allowed_labels_for(member);
    CHECK(has(labels, "public"));
    CHECK(has(labels, "internal"));
    CHECK(has(labels, "confidential"));
    CHECK_FALSE(has(labels, "restricted"));     // fail-closed on the top tier
}

TEST_CASE("clearance: admin power is not a data clearance (still no restricted)",
          "[clearance]")
{
    wikore::Principal admin{.user_id = "a", .email = "a@x", .is_admin = true};
    CHECK_FALSE(has(allowed_labels_for(admin), "restricted"));
}

TEST_CASE("clearance: service account also capped below restricted", "[clearance]")
{
    wikore::Principal svc{.user_id = "s", .email = "s@x", .is_service_account = true};
    CHECK_FALSE(has(allowed_labels_for(svc), "restricted"));
}
