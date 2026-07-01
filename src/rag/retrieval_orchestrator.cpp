#include "wikore/rag/retrieval_orchestrator.hpp"
#include "wikore/rag/qdrant_filter_builder.hpp"
#include "wikore/rag/clearance.hpp"
#include <algorithm>
#include <utility>

namespace wikore::rag {

drogon::Task<Result<std::vector<AllowedCandidate>>>
RetrievalOrchestrator::retrieve(const RequestContext& ctx,
                                std::string           query,
                                std::string_view      scope_org_unit_id,
                                int                   limit) const
{
    if (ctx.deadline_exceeded())
        co_return std::unexpected(Error::unavailable("retrieve: deadline exceeded"));

    const auto& company = ctx.tenant.company_id;

    // 1. embed the query.
    auto vec = co_await embedder_->embed(std::move(query));
    if (!vec) co_return std::unexpected(vec.error());

    // 2. resolve the reader's scope (cached; epoch-validated).
    auto scope = co_await resolver_->resolve(
        company, ctx.principal.user_id, scope_org_unit_id);
    if (!scope) co_return std::unexpected(scope.error());

    // 3. derive clearance - the single enforced sensitivity policy.
    const auto labels = allowed_labels_for(ctx.principal);

    // 4. build the prefilter (fail-closed on empty scope/clearance).
    const auto filter = QdrantFilterBuilder::build(company, *scope, labels);

    // 5. vector search, over-fetched so gate drops do not starve the result.
    const int fetch = std::max(1, limit) * std::max(1, over_fetch_);
    auto candidates = co_await vector_store_->search(*vec, filter, fetch);
    if (!candidates) co_return std::unexpected(candidates.error());

    // 6. EvidenceGate: authoritative live re-validation + hydration. Same
    //    scope and clearance as the prefilter, so the two layers agree.
    auto allowed = co_await gate_.evaluate(company, *scope, labels, *candidates);
    if (!allowed) co_return std::unexpected(allowed.error());

    // Return up to `limit`, in the gate-preserved (retrieval score) order.
    if (static_cast<int>(allowed->size()) > limit)
        allowed->resize(limit);
    co_return std::move(*allowed);
}

} // namespace wikore::rag
