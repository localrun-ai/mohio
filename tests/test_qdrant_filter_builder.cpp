#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/qdrant_filter_builder.hpp"
#include <algorithm>

// Pure unit tests (no DB/Redis) for the Qdrant prefilter builder.

using wikore::rag::QdrantFilterBuilder;
using wikore::rag::SensitivityLevel;
using wikore::rag::sensitivity_labels_up_to;

namespace {
bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}

TEST_CASE("sensitivity_labels_up_to: cumulative by clearance level", "[qdrant_filter]")
{
    CHECK(sensitivity_labels_up_to(SensitivityLevel::public_)
          == std::vector<std::string>{"public"});
    CHECK(sensitivity_labels_up_to(SensitivityLevel::internal)
          == std::vector<std::string>{"public", "internal"});
    CHECK(sensitivity_labels_up_to(SensitivityLevel::confidential)
          == std::vector<std::string>{"public", "internal", "confidential"});
    CHECK(sensitivity_labels_up_to(SensitivityLevel::restricted)
          == std::vector<std::string>{"public", "internal", "confidential", "restricted"});
}

TEST_CASE("QdrantFilterBuilder: maps AccessScope + clearance + tenant to the filter",
          "[qdrant_filter]")
{
    wikore::AccessScope scope;
    scope.org_unit_ids = {"ou-1", "ou-2"};

    auto f = QdrantFilterBuilder::build(
        "co-1", scope, sensitivity_labels_up_to(SensitivityLevel::confidential));

    CHECK(f.company_id == "co-1");
    CHECK(f.access_scope_ids == scope.org_unit_ids);   // reader scope, not owner-only
    CHECK(has(f.sensitivity_labels, "public"));
    CHECK(has(f.sensitivity_labels, "confidential"));
    CHECK_FALSE(has(f.sensitivity_labels, "restricted"));
    CHECK(f.lifecycle_status == "active");             // default
}

TEST_CASE("QdrantFilterBuilder: lifecycle_status override is honoured", "[qdrant_filter]")
{
    wikore::AccessScope scope;
    scope.org_unit_ids = {"ou-1"};
    auto f = QdrantFilterBuilder::build(
        "co-1", scope, sensitivity_labels_up_to(SensitivityLevel::public_), "deprecated");
    CHECK(f.lifecycle_status == "deprecated");
}

TEST_CASE("QdrantFilterBuilder: empty scope yields a fail-closed (match-nothing) filter",
          "[qdrant_filter]")
{
    wikore::AccessScope scope;            // no org_unit_ids
    auto f = QdrantFilterBuilder::build(
        "co-1", scope, sensitivity_labels_up_to(SensitivityLevel::restricted));
    // Empty access_scope_ids -> both vector stores treat the filter as
    // "match nothing" (their documented empty-set guard).
    CHECK(f.access_scope_ids.empty());
}

TEST_CASE("QdrantFilterBuilder: empty clearance yields a fail-closed filter",
          "[qdrant_filter]")
{
    wikore::AccessScope scope;
    scope.org_unit_ids = {"ou-1"};
    auto f = QdrantFilterBuilder::build("co-1", scope, /*allowed=*/{});
    CHECK(f.sensitivity_labels.empty());  // no clearance -> match nothing
}
