#pragma once
#include "wikore/rag/types.hpp"
#include "wikore/domain/types.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// Sensitivity clearance (V014 ordering)
//
// Labels are totally ordered: public < internal < confidential < restricted.
// A reader cleared to level L may see every label up to and including L.
// `sensitivity_labels_up_to` returns that cumulative set, which becomes the
// MatchAny sensitivity filter. (Which level a given principal is cleared to is
// a session/policy decision made by the caller; this is just the label math.)
// ---------------------------------------------------------------------------
enum class SensitivityLevel { public_ = 0, internal = 1, confidential = 2, restricted = 3 };

std::vector<std::string> sensitivity_labels_up_to(SensitivityLevel max_level);

// ---------------------------------------------------------------------------
// QdrantFilterBuilder
//
// Translates a resolved AccessScope + sensitivity clearance + tenant into the
// Qdrant prefilter (docs/iteration_2_design.md section 1 item 3):
//   MatchValue(company_id) AND MatchAny(access_scope_ids)
//   AND MatchAny(sensitivity_labels) AND MatchValue(lifecycle_status).
//
// access_scope_ids is the reader's resolved org_unit set (scope.org_unit_ids),
// matched against each chunk payload's access_scope_ids. NOT owner-only: an
// owner-only filter would miss grant-only visibility (v2.3 note).
//
// Fail-closed by construction: an empty scope or empty label set produces a
// filter whose access_scope_ids / sensitivity_labels is empty, which both
// vector stores treat as "match nothing". The prefilter is a recall/perf fast
// path only; the EvidenceGate (G1) is the authoritative boundary, so prefilter
// staleness can only drop candidates, never leak.
// ---------------------------------------------------------------------------
class QdrantFilterBuilder {
public:
    static QdrantFilter build(std::string_view                company_id,
                              const AccessScope&              scope,
                              const std::vector<std::string>& allowed_sensitivity_labels,
                              std::string_view                lifecycle_status = "active");
};

} // namespace wikore::rag
