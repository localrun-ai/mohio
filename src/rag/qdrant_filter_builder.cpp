#include "wikore/rag/qdrant_filter_builder.hpp"

namespace wikore::rag {

std::vector<std::string> sensitivity_labels_up_to(SensitivityLevel max_level)
{
    std::vector<std::string> labels;
    labels.reserve(4);
    labels.emplace_back("public");                              // always
    if (max_level >= SensitivityLevel::internal)     labels.emplace_back("internal");
    if (max_level >= SensitivityLevel::confidential) labels.emplace_back("confidential");
    if (max_level >= SensitivityLevel::restricted)   labels.emplace_back("restricted");
    return labels;
}

QdrantFilter QdrantFilterBuilder::build(
    std::string_view                company_id,
    const AccessScope&              scope,
    const std::vector<std::string>& allowed_sensitivity_labels,
    std::string_view                lifecycle_status)
{
    QdrantFilter f;
    f.company_id         = std::string(company_id);
    f.access_scope_ids   = scope.org_unit_ids;          // reader's resolved scope
    f.sensitivity_labels = allowed_sensitivity_labels;  // reader's clearance set
    f.lifecycle_status   = std::string(lifecycle_status);
    return f;
}

} // namespace wikore::rag
