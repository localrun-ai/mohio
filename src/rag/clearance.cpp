#include "wikore/rag/clearance.hpp"

namespace wikore::rag {

SensitivityLevel clearance_for(const Principal& /*p*/)
{
    // Every authenticated principal defaults to member clearance
    // (public / internal / confidential). 'restricted' is deliberately NOT
    // granted here: it requires an explicit clearance the schema does not yet
    // model, and V014 forbids restricted content from surfacing without it.
    //
    // Extension seam (single place to change): when a per-principal clearance
    // signal exists, map it here - e.g. a cleared principal -> restricted, a
    // guest/anonymous principal -> public_. is_admin is intentionally NOT a
    // shortcut to restricted; administrative power is not a data clearance.
    return SensitivityLevel::confidential;
}

std::vector<std::string> allowed_labels_for(const Principal& p)
{
    return sensitivity_labels_up_to(clearance_for(p));
}

} // namespace wikore::rag
