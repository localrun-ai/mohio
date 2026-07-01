#pragma once
#include "wikore/rag/qdrant_filter_builder.hpp"   // SensitivityLevel, sensitivity_labels_up_to
#include "wikore/domain/types.hpp"                 // Principal
#include <string>
#include <vector>

namespace wikore::rag {

// ---------------------------------------------------------------------------
// Sensitivity clearance derivation (Iteration 2)
//
// The single enforced point that decides which sensitivity labels a
// principal's session may see. It is consumed by BOTH the Qdrant prefilter and
// the EvidenceGate, so deriving it in one place means the two access-control
// layers can never disagree on policy.
//
// FAIL-CLOSED on the top tier: 'restricted' is NEVER granted by derivation.
// V014 requires restricted content to never surface unless the principal is
// explicitly cleared, and the schema does not yet model a per-principal
// clearance signal - so this caps at 'confidential' (member clearance). When a
// clearance field/grant is added, raise the cap HERE and nowhere else; every
// consumer inherits it automatically.
// ---------------------------------------------------------------------------

// Max sensitivity level this principal may see.
SensitivityLevel clearance_for(const Principal& p);

// The label set the prefilter and gate should use =
// sensitivity_labels_up_to(clearance_for(p)).
std::vector<std::string> allowed_labels_for(const Principal& p);

} // namespace wikore::rag
