#pragma once
#include "wikore/domain/types.hpp"
#include <drogon/drogon.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wikore {

// ---------------------------------------------------------------------------
// AccessResolver (Iteration 2)
//
// Resolves a (tenant, principal, query-scope) to the set of org_unit_ids the
// principal can read - the exact `reader_scope` the retrieval gate consumes
// (G1; docs/iteration_2_design.md section 0). This is the read-path resolver;
// membership/grant CRUD stays in AccessService.
//
// The resolution is a SINGLE SQL statement, so its output is one Read
// Committed snapshot and cannot tear under a concurrent move_org_unit
// (scenario E / section-5 item 3). It mirrors the proven
// AccessService::effective_read_orgs scope logic and additionally captures, in
// the SAME snapshot, the companies.acl_epoch and users.scope_epoch the scope
// was resolved under. Those stamps let the section-2.6 Redis cache validate an
// entry against the live epochs: a stale entry is detectable (and re-resolved)
// even if its lr:eff invalidation was lost.
//
// TODO(iter2-consolidation): AccessService::effective_read_orgs duplicates the
// scope subquery below. Once the cache layer lands, fold effective_read_orgs
// onto this resolver so the security-critical scope SQL has one home.
//
// Returns the domain `AccessScope` (wikore/domain/types.hpp): org_unit_ids is
// the gate's reader_scope; access_epoch (companies.acl_epoch) and scope_epoch
// (users.scope_epoch) are the stamps the section-2.6 cache validates against.
// cache_until is left default by this (uncached) adapter; the cache layer
// computes the TTL clamp (V008) when it lands.
// ---------------------------------------------------------------------------

class AccessResolverPort {
public:
    virtual ~AccessResolverPort() = default;

    virtual drogon::Task<Result<AccessScope>>
    resolve(std::string_view company_id,
            std::string_view user_id,
            std::string_view scope_org_unit_id) const = 0;
};

class PostgresAccessResolver : public AccessResolverPort {
public:
    explicit PostgresAccessResolver(drogon::orm::DbClientPtr db)
        : db_(std::move(db)) {}

    drogon::Task<Result<AccessScope>>
    resolve(std::string_view company_id,
            std::string_view user_id,
            std::string_view scope_org_unit_id) const override;

private:
    drogon::orm::DbClientPtr db_;
};

} // namespace wikore
