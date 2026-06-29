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
// Computes the access_scope_ids snapshot for a document's chunks at ingest.
// Query-time complement: effective_read_orgs() in access.cpp.
//
// SET-ALGEBRA CONTRACT:
//   access_scope_ids stores a RESOLVED set of org_unit IDs. A user may
//   retrieve the chunk if and only if their effective_read_orgs (the set of
//   org_units they are directly a member of, plus descendants via
//   applies_to='self_and_descendants') intersects this set via MatchAny.
//
//   principal_applies_to is handled HERE (ingest side), not query-side:
//     self_only:            store principal_id alone.
//     self_and_descendants: store principal_id + all its descendants via
//                           org_unit_closure so that subtree members match
//                           without any ancestor walk at query time.
//
//   This means effective_read_orgs does NOT walk ancestors -- it only
//   returns org_units the user is effectively a member of. The two sides
//   are consistent: the ingest side expands "who can see" into the stored
//   set; the query side expands "who the user is" into the filter set.
//
// FOUR UNION ARMS:
//   1. Owner:                     owner_org_unit_id.
//   2a. Doc grant self_only:      grant principal_id only.
//   2b. Doc grant s_and_d:        principal_id + all its descendants.
//   3a. Org-unit grant self_only: principal_id, resource subtree check (depth=0).
//   3b. Org-unit grant s_and_d:   principal subtree, resource subtree check.
//
// RESYNC NOTE: this snapshot is computed once at ingest. Grant revocation,
// expiry, owner changes, and org-tree moves leave existing snapshots stale.
// A resync worker that re-runs fetch_access_scopes + Qdrant payload update
// must be triggered by AccessService mutation operations (tracked separately).
// ---------------------------------------------------------------------------

drogon::Task<Result<std::vector<std::string>>>
PostgresDocumentRepo::fetch_access_scopes(std::string_view company_id,
                                           std::string_view document_id)
{
    constexpr auto kSql = R"(
        WITH doc AS (
            SELECT owner_org_unit_id
            FROM   documents
            WHERE  company_id = $1::uuid AND id = $2::uuid
        )
        -- 1. Owner
        SELECT owner_org_unit_id::text AS org_unit_id FROM doc
        UNION
        -- 2a. Document-level grant, principal self_only
        SELECT rg.principal_id::text
        FROM   resource_grants rg
        WHERE  rg.company_id           = $1::uuid
          AND  rg.resource_type        = 'document'
          AND  rg.resource_id          = $2::uuid
          AND  rg.principal_type       = 'org_unit'
          AND  rg.principal_applies_to = 'self_only'
          AND  rg.permission IN ('read','write','admin')
          AND  (rg.expires_at IS NULL OR rg.expires_at > now())
        UNION
        -- 2b. Document-level grant, principal self_and_descendants: expand subtree
        SELECT pc.descendant_id::text
        FROM   resource_grants rg
        JOIN   org_unit_closure pc
            ON pc.company_id  = $1::uuid
           AND pc.ancestor_id = rg.principal_id
        WHERE  rg.company_id           = $1::uuid
          AND  rg.resource_type        = 'document'
          AND  rg.resource_id          = $2::uuid
          AND  rg.principal_type       = 'org_unit'
          AND  rg.principal_applies_to = 'self_and_descendants'
          AND  rg.permission IN ('read','write','admin')
          AND  (rg.expires_at IS NULL OR rg.expires_at > now())
        UNION
        -- 3a. Org-unit-level grant, principal self_only, resource subtree check
        SELECT rg.principal_id::text
        FROM   resource_grants rg
        JOIN   org_unit_closure rc
            ON rc.company_id    = $1::uuid
           AND rc.ancestor_id   = rg.resource_id
           AND rc.descendant_id = (SELECT owner_org_unit_id FROM doc)
        WHERE  rg.company_id           = $1::uuid
          AND  rg.resource_type        = 'org_unit'
          AND  rg.principal_type       = 'org_unit'
          AND  rg.principal_applies_to = 'self_only'
          AND  rg.permission IN ('read','write','admin')
          AND  (rg.expires_at IS NULL OR rg.expires_at > now())
          AND  (
              rg.resource_applies_to = 'self_and_descendants'
           OR (rg.resource_applies_to = 'self_only' AND rc.depth = 0)
          )
        UNION
        -- 3b. Org-unit-level grant, principal self_and_descendants: expand subtree
        SELECT pc.descendant_id::text
        FROM   resource_grants rg
        JOIN   org_unit_closure rc
            ON rc.company_id    = $1::uuid
           AND rc.ancestor_id   = rg.resource_id
           AND rc.descendant_id = (SELECT owner_org_unit_id FROM doc)
        JOIN   org_unit_closure pc
            ON pc.company_id  = $1::uuid
           AND pc.ancestor_id = rg.principal_id
        WHERE  rg.company_id           = $1::uuid
          AND  rg.resource_type        = 'org_unit'
          AND  rg.principal_type       = 'org_unit'
          AND  rg.principal_applies_to = 'self_and_descendants'
          AND  rg.permission IN ('read','write','admin')
          AND  (rg.expires_at IS NULL OR rg.expires_at > now())
          AND  (
              rg.resource_applies_to = 'self_and_descendants'
           OR (rg.resource_applies_to = 'self_only' AND rc.depth = 0)
          )
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

    // Remove orphan sections from a previous attempt that produced a longer
    // layout. Without this, a re-ingest that shrinks the document leaves
    // high-ordinal rows behind with stale heading_path and stale FK targets
    // for document_chunks.section_id. The recursive upsert_section above
    // upserts ordinals 0..ordinal-1; anything at ordinal..N is orphan.
    //
    // Cascade behaviour for the surviving rows:
    //   * document_sections.parent_section_id ON DELETE CASCADE -- deleting
    //     a parent removes its descendants, so a single DELETE covers the
    //     entire orphan subtree even if the recursion produced child rows.
    //   * document_chunks.section_id ON DELETE SET NULL -- chunks that
    //     pointed at an orphan section have their section_id NULLed; the
    //     write_chunks pass that follows then deletes those chunk rows too.
    try {
        co_await uow.exec(
            "DELETE FROM document_sections "
            "WHERE  company_id          = $1::uuid "
            "  AND  document_version_id = $2::uuid "
            "  AND  ordinal             >= $3",
            std::string(company_id),
            std::string(document_version_id),
            ordinal);
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
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

    // Remove orphan chunks from a previous attempt that produced more
    // chunks than this one (re-ingest after a chunker tweak, retry after a
    // partial earlier write). The upsert loop above wrote chunk_index
    // 0..N-1; everything at >= N is orphan with stale content and stale
    // access_scope_ids. document_chunk_vectors has ON DELETE CASCADE on
    // (company_id, chunk_id), so the Postgres-side vector bookkeeping is
    // removed too. The corresponding Qdrant points are NOT explicitly
    // dropped here -- they are bookkept by the deterministic point id
    // (uuid_v5(chunk_id + model)), so they orphan in Qdrant; a separate
    // resync/sweep is the right place to garbage-collect them. This change
    // closes the Postgres-side data-leak (stale content + stale ACLs) and
    // keeps the audit trail correct.
    //
    // Special case: chunks.size() == 0 deletes every row for the version,
    // which is the intended behaviour if a re-parse produced no content.
    const int new_count = static_cast<int>(chunks.size());
    if (!chunks.empty()) {
        try {
            co_await uow.exec(
                "DELETE FROM document_chunks "
                "WHERE  company_id          = $1::uuid "
                "  AND  document_version_id = $2::uuid "
                "  AND  chunk_index         >= $3",
                std::string(company_id),
                std::string(chunks.front().document_version_id),
                new_count);
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

drogon::Task<Result<bool>>
PostgresDocumentRepo::set_ingest_status(std::string_view         company_id,
                                         std::string_view         document_version_id,
                                         IngestStatus             status,
                                         std::string_view         claim_token,
                                         std::string_view         error_message,
                                         drogon::orm::DbClientPtr db)
{
    if (status == IngestStatus::done) {
        co_return std::unexpected(Error::invalid_state(
            "set_ingest_status: use mark_ingest_done() to set 'done' "
            "(it must also set completed_at and chunk_count)"));
    }
    if (status == IngestStatus::processing) {
        co_return std::unexpected(Error::invalid_state(
            "set_ingest_status: use claim_for_processing() for "
            "'processing' (it generates the per-claim ownership token)"));
    }

    const auto status_str = to_string(status);
    // CAS-style predicate:
    //  * always-true id+company_id check
    //  * when claim_token is non-empty, ALSO require the row's stored
    //    token to match -- this is how we prevent a worker that was
    //    reset by the polling fallback from terminally overwriting a
    //    newer worker's state.
    //  * terminal transitions clear ingest_claim_token (the row is no
    //    longer claimed); pending transitions preserve the existing
    //    token (today there is no such caller, but harmless).
    //
    // ATOMICALLY clears ingest_job_payload alongside ingest_claim_token
    // on terminal flips (done/error). Keeping the payload-clear inside
    // the token-gated UPDATE prevents a stale worker from erasing a
    // re-triggered version's recovery state through a separate
    // unconditional UPDATE (which we previously had in worker.cpp's
    // post-Processed cleanup).
    constexpr auto kSql = R"(
        UPDATE document_versions
        SET    ingest_status      = $3,
               error_msg          = CASE WHEN $3 = 'error'
                                         THEN NULLIF($5, '') ELSE NULL END,
               ingest_claim_token = CASE WHEN $3 IN ('error', 'done')
                                         THEN NULL ELSE ingest_claim_token END,
               ingest_job_payload = CASE WHEN $3 IN ('error', 'done')
                                         THEN NULL ELSE ingest_job_payload END
        WHERE  company_id = $1::uuid
          AND  id         = $2::uuid
          AND  ($4 = '' OR ingest_claim_token::text = $4)
        RETURNING id
    )";

    try {
        auto rows = co_await db->execSqlCoro(kSql,
                                              std::string(company_id),
                                              std::string(document_version_id),
                                              std::string(status_str),
                                              std::string(claim_token),
                                              std::string(error_message));
        co_return rows.size() == 1;
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
}

drogon::Task<Result<std::optional<std::string>>>
PostgresDocumentRepo::claim_for_processing(std::string_view         company_id,
                                            std::string_view         document_version_id,
                                            std::string_view         payload_json,
                                            drogon::orm::DbClientPtr db)
{
    // Atomic CAS that:
    //   * flips ingest_status from 'pending' to 'processing'
    //   * persists ingest_job_payload (NULL if caller passed empty)
    //   * generates a fresh ingest_claim_token (UUID) and RETURNs it,
    //     so the caller can present it to mark_ingest_done /
    //     set_ingest_status('error') as proof of ownership
    //   * gates on lifecycle_status so archived versions cannot be
    //     re-ingested (V010 treats archived as terminal-no-retrieval)
    //
    // Returns std::nullopt if the row is no longer claimable (already
    // processing/done/error/archived), in which case the caller should
    // ack the delivery as a duplicate.
    constexpr auto kSql = R"(
        UPDATE document_versions
        SET    ingest_status      = 'processing',
               ingest_job_payload = NULLIF($3, '')::jsonb,
               ingest_claim_token = gen_random_uuid()
        WHERE  company_id        = $1::uuid
          AND  id                = $2::uuid
          AND  ingest_status     = 'pending'
          AND  lifecycle_status  IN ('draft', 'deprecated')
        RETURNING ingest_claim_token::text AS token
    )";
    try {
        auto rows = co_await db->execSqlCoro(kSql,
                                              std::string(company_id),
                                              std::string(document_version_id),
                                              std::string(payload_json));
        if (rows.empty())
            co_return std::optional<std::string>{};
        co_return std::optional<std::string>{
            rows[0]["token"].as<std::string>()};
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
}

drogon::Task<Result<bool>>
PostgresDocumentRepo::mark_ingest_done(std::string_view      company_id,
                                       std::string_view      document_version_id,
                                       int                   chunk_count,
                                       std::string_view      claim_token,
                                       postgres::UnitOfWork& uow)
{
    // Ownership-gated:
    //   * empty claim_token -> bypass (test/legacy path)
    //   * non-empty claim_token -> require match. If the polling
    //     fallback reset our processing claim and another worker took
    //     over, our token no longer matches and this UPDATE matches 0
    //     rows. The caller (use case) detects via the returned bool
    //     and propagates as OwnershipLost outcome.
    //
    // ATOMICALLY clears ingest_claim_token AND ingest_job_payload as
    // part of the terminal flip. Doing the payload-clear here (token-
    // gated) instead of in a separate post-commit UPDATE prevents a
    // stale worker from clearing a re-triggered version's recovery
    // state via an unconditional UPDATE.
    constexpr auto kSql = R"(
        UPDATE document_versions
        SET    ingest_status      = 'done',
               completed_at       = COALESCE(completed_at, now()),
               chunk_count        = $3,
               ingest_claim_token = NULL,
               ingest_job_payload = NULL
        WHERE  company_id = $1::uuid
          AND  id         = $2::uuid
          AND  ($4 = '' OR ingest_claim_token::text = $4)
        RETURNING id
    )";

    try {
        auto rows = co_await uow.exec(std::string(kSql),
            std::string(company_id),
            std::string(document_version_id),
            chunk_count,
            std::string(claim_token));
        co_return rows.size() == 1;
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }
}

} // namespace wikore::ingest
