#pragma once
#include "wikore/ingest/types.hpp"
#include "wikore/adapters/postgres/unit_of_work.hpp"
#include "wikore/domain/types.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <map>
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
                 const std::vector<std::string>& access_scope_ids,
                 postgres::UnitOfWork&           uow) = 0;

    // Update document_versions.ingest_status to an intermediate state
    // ('pending', 'processing', or 'error'). 'done' is rejected -- callers
    // must use mark_ingest_done() instead, because the 'done' state CHECK
    // also requires completed_at AND chunk_count to be non-NULL.
    //
    // Runs on `db` (typically outside any UoW) so a failure flip survives
    // even when the main ingest transaction rolls back.
    virtual drogon::Task<Result<void>>
    set_ingest_status(std::string_view company_id,
                      std::string_view document_version_id,
                      IngestStatus     status,
                      drogon::orm::DbClientPtr db) = 0;

    // Transition to 'done' inside the provided UoW. Satisfies V003's
    // document_versions_done_state_chk (ingest_status='done' requires
    // completed_at IS NOT NULL AND chunk_count IS NOT NULL).
    virtual drogon::Task<Result<void>>
    mark_ingest_done(std::string_view      company_id,
                     std::string_view      document_version_id,
                     int                   chunk_count,
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
                 const std::vector<std::string>& access_scope_ids,
                 postgres::UnitOfWork&           uow) override;

    drogon::Task<Result<void>>
    set_ingest_status(std::string_view         company_id,
                      std::string_view         document_version_id,
                      IngestStatus             status,
                      drogon::orm::DbClientPtr db) override;

    drogon::Task<Result<void>>
    mark_ingest_done(std::string_view      company_id,
                     std::string_view      document_version_id,
                     int                   chunk_count,
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
                 const std::vector<std::string>& /*access_scope_ids*/,
                 postgres::UnitOfWork&           /*uow*/) override
    {
        for (auto& c : chunks)
            c.db_id = "null-chunk-" + std::to_string(c.chunk_index);
        co_return Result<void>{};
    }

    drogon::Task<Result<void>>
    set_ingest_status(std::string_view         /*company_id*/,
                      std::string_view         version_id,
                      IngestStatus             status,
                      drogon::orm::DbClientPtr /*db*/) override
    {
        // Mirror the production guard so tests catch incorrect usage early.
        if (status == IngestStatus::done) {
            co_return std::unexpected(Error::invalid_state(
                "set_ingest_status: use mark_ingest_done() for 'done'"));
        }
        last_status_set[std::string(version_id)] = status;
        co_return Result<void>{};
    }

    drogon::Task<Result<void>>
    mark_ingest_done(std::string_view      /*company_id*/,
                     std::string_view      version_id,
                     int                   chunk_count,
                     postgres::UnitOfWork& /*uow*/) override
    {
        last_status_set[std::string(version_id)] = IngestStatus::done;
        last_chunk_count[std::string(version_id)] = chunk_count;
        co_return Result<void>{};
    }

    // Test introspection
    std::map<std::string, IngestStatus> last_status_set;
    std::map<std::string, int>          last_chunk_count;

private:
    static void assign_fake_ids(ParsedSection& s)
    {
        s.db_id = "null-section-" + s.heading;
        for (auto& child : s.children)
            assign_fake_ids(child);
    }
};

} // namespace wikore::ingest
