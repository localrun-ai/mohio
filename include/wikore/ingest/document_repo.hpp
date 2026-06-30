#pragma once
#include "wikore/ingest/types.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// DocumentRepoPort: persistence interface for the ingest pipeline.
//
// Implementations: PostgresDocumentRepo (production), NullDocumentRepo (tests).
// ---------------------------------------------------------------------------

class DocumentRepoPort {
public:
    virtual ~DocumentRepoPort() = default;

    // Fetch the access scope org_unit_ids for a document.
    // Returns the owner org_unit plus all org_units that have a resource_grant
    // with document_read access on this document.
    virtual drogon::Task<Result<std::vector<std::string>>>
    fetch_access_scopes(std::string_view company_id,
                        std::string_view document_id) = 0;

    // Write ParsedSection tree to document_sections table (idempotent upsert).
    // Populates section.db_id in-place. Runs inside the provided UoW.
    virtual drogon::Task<Result<void>>
    write_sections(ParsedDocument&          doc,
                   std::string_view         document_version_id,
                   std::string_view         company_id,
                   postgres::UnitOfWork&    uow) = 0;

    // Upsert document_chunks rows and populate Chunk::db_id in-place.
    // ON CONFLICT (document_version_id, chunk_index) DO UPDATE -- V003's
    // actual unique index does NOT include company_id (composite FK enforces
    // tenancy separately). access_scope_ids is denormalized into every chunk
    // row so the Qdrant payload resync job can be driven off the chunk table.
    virtual drogon::Task<Result<void>>
    write_chunks(std::vector<Chunk>&             chunks,
                 std::string_view                company_id,
                 std::string_view                document_version_id,
                 const std::vector<std::string>& access_scope_ids,
                 postgres::UnitOfWork&           uow) = 0;

    // Update document_versions.ingest_status to an intermediate state.
    //
    // 'done' is rejected (callers must use mark_ingest_done -- the
    // 'done' state CHECK also requires completed_at AND chunk_count to
    // be non-NULL).
    //
    // 'error' transitions are GUARDED by the claim token: if `token`
    // is non-empty, the UPDATE only fires when document_versions
    // .ingest_claim_token = token. A mismatch returns Ok(false), which
    // the use case interprets as OwnershipLost (the polling fallback
    // reset our claim and another worker has it now). Empty `token`
    // means "test path; no ownership check" -- production code paths
    // ALWAYS pass a token. Token comes from claim_for_processing's
    // RETURNING clause.
    //
    // 'pending' is allowed (e.g. tests staging a row) but is not used
    // by production code paths after V029.
    //
    // 'processing' transitions are NOT permitted via set_ingest_status
    // -- use claim_for_processing instead so a token is generated and
    // stored atomically with the flip.
    //
    // Runs on `db` (typically outside any UoW) so a failure flip
    // survives even when the main ingest transaction rolls back.
    //
    // Returns Ok(true)  if the UPDATE affected 1 row (we own the
    //                   transition).
    // Returns Ok(false) if the token check failed (ownership lost).
    // Returns Err on DB error.
    virtual drogon::Task<Result<bool>>
    set_ingest_status(std::string_view         company_id,
                      std::string_view         document_version_id,
                      IngestStatus             status,
                      std::string_view         claim_token,  // empty = no check
                      std::string_view         error_message,
                      drogon::orm::DbClientPtr db) = 0;

    // Atomic claim transition: pending -> processing with payload persist
    // AND generation of a per-claim ownership token (UUID). The token is
    // returned and must be threaded to subsequent terminal mutations
    // (mark_ingest_done, set_ingest_status('error')) so a worker whose
    // claim was reset by the polling fallback cannot overwrite a newer
    // worker's terminal state.
    //
    // CAS predicate also checks lifecycle_status IN ('draft','deprecated')
    // so archived versions (V010 treats archived as terminal-no-retrieval)
    // cannot be re-ingested.
    //
    // Returns:
    //   Ok(token)     -- claim won. Pass the token to mark_*/error.
    //   Ok(nullopt)   -- claim lost: row was not pending/draft (already
    //                    processing, done, error, or archived). Caller
    //                    should LREM the proc entry without processing.
    //   Err           -- DB error.
    virtual drogon::Task<Result<std::optional<std::string>>>
    claim_for_processing(std::string_view         company_id,
                         std::string_view         document_version_id,
                         std::string_view         payload_json,
                         drogon::orm::DbClientPtr db) = 0;

    // Transition to 'done' inside the provided UoW. Satisfies V003's
    // document_versions_done_state_chk (ingest_status='done' requires
    // completed_at IS NOT NULL AND chunk_count IS NOT NULL).
    //
    // Ownership-gated via the claim_token: an empty token bypasses the
    // check (test path); a non-empty token requires
    // document_versions.ingest_claim_token = token. A mismatch returns
    // Ok(false) (OwnershipLost) and the caller MUST roll back the UoW.
    virtual drogon::Task<Result<bool>>
    mark_ingest_done(std::string_view      company_id,
                     std::string_view      document_version_id,
                     int                   chunk_count,
                     std::string_view      claim_token,
                     postgres::UnitOfWork& uow) = 0;
};

// ---------------------------------------------------------------------------
// PostgresDocumentRepo
// ---------------------------------------------------------------------------

class PostgresDocumentRepo : public DocumentRepoPort {
public:
    explicit PostgresDocumentRepo(drogon::orm::DbClientPtr db)
        : _db(std::move(db)) {}

    drogon::Task<Result<std::vector<std::string>>>
    fetch_access_scopes(std::string_view company_id,
                        std::string_view document_id) override;

    drogon::Task<Result<void>>
    write_sections(ParsedDocument&       doc,
                   std::string_view      document_version_id,
                   std::string_view      company_id,
                   postgres::UnitOfWork& uow) override;

    drogon::Task<Result<void>>
    write_chunks(std::vector<Chunk>&             chunks,
                 std::string_view                company_id,
                 std::string_view                document_version_id,
                 const std::vector<std::string>& access_scope_ids,
                 postgres::UnitOfWork&           uow) override;

    drogon::Task<Result<bool>>
    set_ingest_status(std::string_view         company_id,
                      std::string_view         document_version_id,
                      IngestStatus             status,
                      std::string_view         claim_token,
                      std::string_view         error_message,
                      drogon::orm::DbClientPtr db) override;

    drogon::Task<Result<std::optional<std::string>>>
    claim_for_processing(std::string_view         company_id,
                         std::string_view         document_version_id,
                         std::string_view         payload_json,
                         drogon::orm::DbClientPtr db) override;

    drogon::Task<Result<bool>>
    mark_ingest_done(std::string_view      company_id,
                     std::string_view      document_version_id,
                     int                   chunk_count,
                     std::string_view      claim_token,
                     postgres::UnitOfWork& uow) override;

private:
    drogon::orm::DbClientPtr _db;

    // Recursive helper: upsert one section and its children, filling db_id
    // and threading the parent's heading_path so each row's heading_path
    // is the full breadcrumb from the root.
    drogon::Task<Result<void>>
    upsert_section(ParsedSection&                    section,
                   std::string_view                  document_version_id,
                   std::string_view                  company_id,
                   const std::optional<std::string>& parent_section_id,
                   const std::vector<std::string>&   parent_heading_path,
                   int&                              ordinal,
                   postgres::UnitOfWork&             uow);
};

// ---------------------------------------------------------------------------
// NullDocumentRepo: in-memory stub for unit tests.
// ---------------------------------------------------------------------------

class NullDocumentRepo : public DocumentRepoPort {
public:
    // Pre-seeded access scopes returned for every document.
    std::vector<std::string> access_scopes{"org-unit-test-1"};

    drogon::Task<Result<std::vector<std::string>>>
    fetch_access_scopes(std::string_view /*company_id*/,
                        std::string_view /*document_id*/) override
    {
        co_return access_scopes;
    }

    drogon::Task<Result<void>>
    write_sections(ParsedDocument& doc,
                   std::string_view /*document_version_id*/,
                   std::string_view /*company_id*/,
                   postgres::UnitOfWork& /*uow*/) override
    {
        // Assign fake db_ids so chunker gets section_id values
        for (auto& s : doc.sections)
            assign_fake_ids(s);
        co_return Result<void>{};
    }

    drogon::Task<Result<void>>
    write_chunks(std::vector<Chunk>&             chunks,
                 std::string_view                /*company_id*/,
                 std::string_view                /*document_version_id*/,
                 const std::vector<std::string>& /*access_scope_ids*/,
                 postgres::UnitOfWork&           /*uow*/) override
    {
        for (auto& c : chunks)
            c.db_id = "null-chunk-" + std::to_string(c.chunk_index);
        co_return Result<void>{};
    }

    drogon::Task<Result<bool>>
    set_ingest_status(std::string_view         /*company_id*/,
                      std::string_view         version_id,
                      IngestStatus             status,
                      std::string_view         claim_token,
                      std::string_view         error_message,
                      drogon::orm::DbClientPtr /*db*/) override
    {
        // Mirror the production guard so tests catch incorrect usage early.
        if (status == IngestStatus::done) {
            co_return std::unexpected(Error::invalid_state(
                "set_ingest_status: use mark_ingest_done() for 'done'"));
        }
        if (status == IngestStatus::processing) {
            co_return std::unexpected(Error::invalid_state(
                "set_ingest_status: use claim_for_processing() for "
                "'processing' (it generates the claim token)"));
        }
        // Ownership check: when a token is provided, only allow the
        // mutation if it matches the stored token for this version.
        if (!claim_token.empty()) {
            auto it = last_claim_token.find(std::string(version_id));
            if (it == last_claim_token.end() || it->second != claim_token) {
                co_return false;   // OwnershipLost
            }
        }
        last_status_set[std::string(version_id)] = status;
        if (status == IngestStatus::error)
            last_error_set[std::string(version_id)] = error_message;
        // Terminal flip clears the token (the row is no longer claimed).
        last_claim_token.erase(std::string(version_id));
        co_return true;
    }

    drogon::Task<Result<std::optional<std::string>>>
    claim_for_processing(std::string_view         /*company_id*/,
                         std::string_view         version_id,
                         std::string_view         /*payload_json*/,
                         drogon::orm::DbClientPtr /*db*/) override
    {
        // Stub mirrors the CAS: if the row is currently 'pending'
        // (the test default after set_ingest_status), claim it.
        auto it = last_status_set.find(std::string(version_id));
        const auto cur = (it == last_status_set.end())
            ? IngestStatus::pending : it->second;
        if (cur != IngestStatus::pending)
            co_return std::optional<std::string>{};
        last_status_set[std::string(version_id)] = IngestStatus::processing;
        // Generate a deterministic-ish token for the stub. The token is
        // arbitrary; what matters is that subsequent mutations must
        // present the same value.
        std::string token = std::string(version_id) + ":claim";
        last_claim_token[std::string(version_id)] = token;
        co_return std::optional<std::string>{std::move(token)};
    }

    drogon::Task<Result<bool>>
    mark_ingest_done(std::string_view      /*company_id*/,
                     std::string_view      version_id,
                     int                   chunk_count,
                     std::string_view      claim_token,
                     postgres::UnitOfWork& /*uow*/) override
    {
        // Same token check as production.
        if (!claim_token.empty()) {
            auto it = last_claim_token.find(std::string(version_id));
            if (it == last_claim_token.end() || it->second != claim_token) {
                co_return false;   // OwnershipLost
            }
        }
        last_status_set[std::string(version_id)] = IngestStatus::done;
        last_chunk_count[std::string(version_id)] = chunk_count;
        last_claim_token.erase(std::string(version_id));
        co_return true;
    }

    // Test introspection
    std::map<std::string, IngestStatus> last_status_set;
    std::map<std::string, int>          last_chunk_count;
    std::map<std::string, std::string>  last_claim_token;
    std::map<std::string, std::string>  last_error_set;

private:
    static void assign_fake_ids(ParsedSection& s)
    {
        s.db_id = "null-section-" + s.heading;
        for (auto& child : s.children)
            assign_fake_ids(child);
    }
};

} // namespace wikore::ingest
