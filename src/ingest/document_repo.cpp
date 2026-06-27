#include "wikore/ingest/document_repo.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <format>

namespace wikore::ingest {

namespace {

// SHA-256 hex of a string. Used to populate document_chunks.content_hash,
// which V003 declares NOT NULL.
std::string sha256_hex(std::string_view s)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    bool ok =
           EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
        && EVP_DigestUpdate(ctx, s.data(), s.size())    == 1
        && EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return {};
    std::string out;
    out.reserve(digest_len * 2);
    static constexpr char kHex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < digest_len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0xf]);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// PostgresDocumentRepo::fetch_access_scopes
//
// Returns the owner org_unit_id of the document plus all org_unit_ids that
// have at least one resource_grant with permission >= 'read' on this doc.
// V002's resource_grants schema (the one on main) uses principal_id rather
// than grantee_org_unit_id and permission rather than grant_type.
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
            WHERE  company_id = $1::uuid AND id = $2::uuid
            UNION
            SELECT rg.principal_id
            FROM   resource_grants rg
            WHERE  rg.company_id     = $1::uuid
              AND  rg.resource_type  = 'document'
              AND  rg.resource_id    = $2::uuid
              AND  rg.principal_type = 'org_unit'
              AND  rg.permission IN ('read','write','admin')
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
// PostgresDocumentRepo::upsert_section / write_sections
//
// V014 document_sections columns:
//     parent_section_id  (NOT id; NULL for top-level)
//     ordinal            (NOT NULL, document-global sequential position)
//     heading            (TEXT; NULL allowed before first heading)
//     heading_path       (TEXT[] NOT NULL DEFAULT '{}'; breadcrumb)
//     depth              (NOT NULL CHECK >= 0)
//
// Idempotency: V014 declares UNIQUE (company_id, document_version_id, ordinal),
// so re-ingest with the same ordinal layout is upsert-safe by (... , ordinal).
// Section content lives in chunks (document_chunks.content), not on the
// section row -- there is no `body` column.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
PostgresDocumentRepo::upsert_section(ParsedSection&                    section,
                                      std::string_view                  document_version_id,
                                      std::string_view                  company_id,
                                      const std::optional<std::string>& parent_section_id,
                                      const std::vector<std::string>&   parent_heading_path,
                                      int&                              ordinal,
                                      postgres::UnitOfWork&             uow)
{
    constexpr auto kSql = R"(
        INSERT INTO document_sections
               (company_id, document_version_id, parent_section_id,
                ordinal, heading, heading_path, depth)
        VALUES ($1::uuid, $2::uuid, $3::uuid,
                $4, $5, $6::text[], $7)
        ON CONFLICT (company_id, document_version_id, ordinal)
        DO UPDATE SET
            parent_section_id = EXCLUDED.parent_section_id,
            heading           = EXCLUDED.heading,
            heading_path      = EXCLUDED.heading_path,
            depth             = EXCLUDED.depth
        RETURNING id::text
    )";

    // Build heading_path = parent's path + this section's heading.
    auto heading_path = parent_heading_path;
    if (!section.heading.empty())
        heading_path.push_back(section.heading);

    // Postgres TEXT[] literal: {"foo","bar"}. Escape \ and " inside values.
    auto pg_text_array = [](const std::vector<std::string>& v) {
        std::string out = "{";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            out += '"';
            for (char c : v[i]) {
                if (c == '\\' || c == '"') out += '\\';
                out += c;
            }
            out += '"';
        }
        out += '}';
        return out;
    };

    const auto path_literal = pg_text_array(heading_path);
    const std::optional<std::string> heading_opt =
        section.heading.empty() ? std::nullopt
                                : std::optional<std::string>{section.heading};

    try {
        auto rows = co_await uow.exec(kSql,
            std::string(company_id),
            std::string(document_version_id),
            parent_section_id,
            ordinal++,
            heading_opt,
            path_literal,
            section.depth);

        if (rows.empty()) {
            co_return std::unexpected(
                Error::database_error("write_sections: no row returned from upsert"));
        }
        section.db_id = rows[0]["id"].as<std::string>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    for (auto& child : section.children) {
        if (auto res = co_await upsert_section(
                child, document_version_id, company_id,
                section.db_id, heading_path, ordinal, uow);
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
    int ordinal = 0;
    const std::vector<std::string> empty_path;
    for (auto& section : doc.sections) {
        if (auto res = co_await upsert_section(
                section, document_version_id, company_id,
                std::nullopt, empty_path, ordinal, uow);
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
// V003 document_chunks columns: company_id, document_version_id, chunk_index,
//                                content (NOT text_content), content_hash
//                                (NOT NULL, SHA-256), access_scope_ids,
//                                section_id (V014).
// V003 unique index: (document_version_id, chunk_index)  -- no company_id.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
PostgresDocumentRepo::write_chunks(std::vector<Chunk>&             chunks,
                                    std::string_view                company_id,
                                    const std::vector<std::string>& access_scope_ids,
                                    postgres::UnitOfWork&           uow)
{
    constexpr auto kSql = R"(
        INSERT INTO document_chunks
               (company_id, document_version_id, chunk_index,
                content, content_hash, access_scope_ids, section_id)
        VALUES ($1::uuid, $2::uuid, $3,
                $4, $5, $6::uuid[], $7::uuid)
        ON CONFLICT (document_version_id, chunk_index)
        DO UPDATE SET
            content          = EXCLUDED.content,
            content_hash     = EXCLUDED.content_hash,
            access_scope_ids = EXCLUDED.access_scope_ids,
            section_id       = EXCLUDED.section_id
        RETURNING id::text
    )";

    // Postgres UUID[] literal: {"<uuid>",...}. The values are validated
    // UUIDs (computed by the resolver upstream); no escaping needed beyond
    // quoting.
    auto pg_uuid_array = [](const std::vector<std::string>& v) {
        std::string out = "{";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            out += '"';
            out += v[i];
            out += '"';
        }
        out += '}';
        return out;
    };
    const auto scope_literal = pg_uuid_array(access_scope_ids);

    for (auto& chunk : chunks) {
        try {
            const auto content_hash = sha256_hex(chunk.text);
            if (content_hash.empty()) {
                co_return std::unexpected(
                    Error::database_error("write_chunks: content_hash digest failed"));
            }
            auto rows = co_await uow.exec(kSql,
                std::string(company_id),
                chunk.document_version_id,
                chunk.chunk_index,
                chunk.text,
                content_hash,
                scope_literal,
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
// Two overloads:
//   * (intermediate) updates ingest_status alone -- legal for the
//     'pending' -> 'processing' transition. Used outside the UoW so a
//     'processing' flip survives even if the ingest later fails and the
//     main transaction is rolled back (we still want the row to show the
//     attempt happened).
//   * (terminal_done) when transitioning to 'done', the v003
//     document_versions_done_state_chk constraint requires completed_at
//     AND chunk_count to be non-NULL. Caller passes both.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>>
PostgresDocumentRepo::set_ingest_status(std::string_view         company_id,
                                         std::string_view         document_version_id,
                                         IngestStatus             status,
                                         drogon::orm::DbClientPtr db,
                                         std::string_view         error_message)
{
    if (status == IngestStatus::done) {
        co_return std::unexpected(Error::invalid_state(
            "set_ingest_status: use mark_ingest_done() to set 'done' "
            "(it must also set completed_at and chunk_count)"));
    }

    const auto status_str = to_string(status);
    constexpr auto kSql = R"(
        UPDATE document_versions
        SET    ingest_status = $3,
               error_msg = CASE WHEN $3 = 'error' THEN NULLIF($4, '') ELSE NULL END
        WHERE  company_id = $1::uuid AND id = $2::uuid
    )";

    try {
        co_await db->execSqlCoro(kSql,
                                  std::string(company_id),
                                  std::string(document_version_id),
                                  std::string(status_str),
                                  std::string(error_message));
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return Result<void>{};
}

drogon::Task<Result<void>>
PostgresDocumentRepo::mark_ingest_done(std::string_view      company_id,
                                       std::string_view      document_version_id,
                                       int                   chunk_count,
                                       postgres::UnitOfWork& uow)
{
    constexpr auto kSql = R"(
        UPDATE document_versions
        SET    ingest_status = 'done',
               completed_at  = COALESCE(completed_at, now()),
               chunk_count   = $3
        WHERE  company_id = $1::uuid AND id = $2::uuid
    )";

    try {
        co_await uow.exec(kSql,
            std::string(company_id),
            std::string(document_version_id),
            chunk_count);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
    co_return Result<void>{};
}

} // namespace wikore::ingest
