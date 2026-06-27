#include "wikore/ingest/document_repo.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <spdlog/spdlog.h>
#include <format>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// PostgresDocumentRepo::fetch_access_scopes
//
// Returns the owner org_unit_id of the document plus all org_unit_ids that
// have at least one resource_grant with grant_type 'document_read' on this doc.
// ---------------------------------------------------------------------------

drogon::Task<Result<std::vector<std::string>>>
PostgresDocumentRepo::fetch_access_scopes(std::string_view company_id,
                                           std::string_view document_id)
{
    constexpr auto kSql = R"(
        SELECT org_unit_id
        FROM (
            SELECT owner_org_unit_id AS org_unit_id
            FROM   documents
            WHERE  company_id = $1 AND id = $2::uuid
            UNION
            SELECT rg.grantee_org_unit_id
            FROM   resource_grants rg
            WHERE  rg.company_id   = $1
              AND  rg.resource_id  = $2::uuid
              AND  rg.grant_type   = 'document_read'
              AND  (rg.expires_at IS NULL OR rg.expires_at > now())
        ) t
    )";

    try {
        auto rows = co_await _db->execSqlCoro(kSql,
                                              std::string(company_id),
                                              std::string(document_id));
        std::vector<std::string> scopes;
        scopes.reserve(rows.size());
        for (const auto& row : rows)
            scopes.push_back(row["org_unit_id"].as<std::string>());
        co_return scopes;
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
}

// ---------------------------------------------------------------------------
// PostgresDocumentRepo::write_sections
//
// Idempotent upsert into document_sections. Recursive - children are written
// after their parent so parent db_id is available for the FK.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
PostgresDocumentRepo::upsert_section(ParsedSection&                    section,
                                      std::string_view                  document_version_id,
                                      std::string_view                  company_id,
                                      const std::optional<std::string>& parent_id,
                                      int&                              position,
                                      postgres::UnitOfWork&             uow)
{
    constexpr auto kSql = R"(
        INSERT INTO document_sections
               (company_id, document_version_id, parent_id,
                heading, depth, position, body)
        VALUES ($1, $2::uuid, $3::uuid,
                $4, $5, $6, $7)
        ON CONFLICT (company_id, document_version_id, heading, depth, position)
        DO UPDATE SET body = EXCLUDED.body
        RETURNING id::text
    )";

    try {
        auto rows = co_await uow.exec(kSql,
            std::string(company_id),
            std::string(document_version_id),
            parent_id,
            section.heading,
            section.depth,
            position++,
            section.text);

        if (rows.empty()) {
            co_return std::unexpected(
                Error::database_error("write_sections: no row returned from upsert"));
        }
        section.db_id = rows[0]["id"].as<std::string>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    int child_pos = 0;
    for (auto& child : section.children) {
        if (auto res = co_await upsert_section(
                child, document_version_id, company_id,
                section.db_id, child_pos, uow);
            !res)
        {
            co_return res;
        }
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
PostgresDocumentRepo::write_sections(ParsedDocument&        doc,
                                      std::string_view       document_version_id,
                                      std::string_view       company_id,
                                      postgres::UnitOfWork&  uow)
{
    int top_pos = 0;
    for (auto& section : doc.sections) {
        if (auto res = co_await upsert_section(
                section, document_version_id, company_id,
                std::nullopt, top_pos, uow);
            !res)
        {
            co_return res;
        }
    }
    co_return Result<void>{};
}

// ---------------------------------------------------------------------------
// PostgresDocumentRepo::write_chunks
//
// Upsert document_chunks. ON CONFLICT by (company_id, document_version_id,
// chunk_index) updates the text and section reference (re-ingest may change
// chunk boundaries if the source file changed).
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
PostgresDocumentRepo::write_chunks(std::vector<Chunk>&   chunks,
                                    std::string_view      company_id,
                                    postgres::UnitOfWork& uow)
{
    constexpr auto kSql = R"(
        INSERT INTO document_chunks
               (company_id, document_version_id, chunk_index,
                text_content, section_id)
        VALUES ($1, $2::uuid, $3, $4, $5::uuid)
        ON CONFLICT (company_id, document_version_id, chunk_index)
        DO UPDATE SET
            text_content = EXCLUDED.text_content,
            section_id   = EXCLUDED.section_id
        RETURNING id::text
    )";

    for (auto& chunk : chunks) {
        try {
            auto rows = co_await uow.exec(kSql,
                std::string(company_id),
                chunk.document_version_id,
                chunk.chunk_index,
                chunk.text,
                chunk.section_id);

            if (!rows.empty())
                chunk.db_id = rows[0]["id"].as<std::string>();
        } catch (const drogon::orm::DrogonDbException& ex) {
            co_return std::unexpected(postgres::map_db_exception(ex));
        }
    }
    co_return Result<void>{};
}

// ---------------------------------------------------------------------------
// PostgresDocumentRepo::set_ingest_status
//
// Runs outside the ingest transaction so failures can be recorded even when
// the main transaction was rolled back.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
PostgresDocumentRepo::set_ingest_status(std::string_view         company_id,
                                         std::string_view         document_version_id,
                                         IngestStatus             status,
                                         drogon::orm::DbClientPtr db)
{
    const auto status_str = to_string(status);
    constexpr auto kSql = R"(
        UPDATE document_versions
        SET    ingest_status = $3
        WHERE  company_id = $1 AND id = $2::uuid
    )";

    try {
        co_await db->execSqlCoro(kSql,
                                  std::string(company_id),
                                  std::string(document_version_id),
                                  std::string(status_str));
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return Result<void>{};
}

} // namespace wikore::ingest
