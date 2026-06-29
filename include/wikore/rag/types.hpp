#pragma once
#include "wikore/domain/types.hpp"
#include <openssl/evp.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::rag {

using Embedding = std::vector<float>;

// ---------------------------------------------------------------------------
// UUID v5 for deterministic Qdrant point IDs.
//
// Point ID = uuid_v5(url_namespace, chunk_id + ":" + embed_model_id)
// This means: same chunk + same model = same point ID across re-ingests,
// which makes upsert naturally idempotent.
//
// Returns std::nullopt on any OpenSSL failure (allocation or digest). The
// previous version returned the all-zero UUID sentinel, which silently
// collided every chunk in a batch onto a single point id -- all but one
// would vanish from Qdrant on upsert with no error surfaced. Returning
// nullopt forces the caller to handle the failure (typically by failing
// the outbox event so the next retry sees a healthy allocator).
// ---------------------------------------------------------------------------

inline std::optional<std::string> uuid_v5(std::string_view name) {
    // RFC 4122 URL namespace: 6ba7b811-9dad-11d1-80b4-00c04fd430c8
    static constexpr uint8_t kNs[16] = {
        0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8,
    };
    uint8_t      hash[20] = {0};
    unsigned int hash_len = sizeof(hash);

    // Use EVP API (OpenSSL 3.0+; SHA-1 low-level API is deprecated).
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return std::nullopt;

    bool ok =
           EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1
        && EVP_DigestUpdate(ctx, kNs, 16)              == 1
        && EVP_DigestUpdate(ctx, name.data(), name.size()) == 1
        && EVP_DigestFinal_ex(ctx, hash, &hash_len)    == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return std::nullopt;

    // Set version=5 and RFC 4122 variant bits.
    hash[6] = (hash[6] & 0x0f) | 0x50;
    hash[8] = (hash[8] & 0x3f) | 0x80;
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        hash[0], hash[1], hash[2], hash[3],
        hash[4], hash[5], hash[6], hash[7],
        hash[8], hash[9],
        hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// ChunkPayload: every field stored as a Qdrant payload attribute.
//
// All fields are queryable as Qdrant filters without touching Postgres.
// Bump kSchemaVersion when adding/removing fields; the resync worker uses
// this to identify stale points that need payload refresh.
//
// V003 contract: the canonical names are document_id (struct) and
// owner_org_unit_id (the document's home OU). authority_level (0..100)
// comes from documents.authority_level and is consumed by the reranker
// to weight high-authority chunks (policy, mandate) above informational
// ones (notes, drafts). Both fields are written at ingest time and never
// mutated for an existing point id; if a document changes owner, ingest
// produces a new document_version and a new point id.
// ---------------------------------------------------------------------------

struct ChunkPayload {
    static constexpr int kSchemaVersion = 2;

    std::string              company_id;
    std::string              document_id;
    std::string              document_version_id;
    // Optional because schema-v1 points written before this field existed
    // have no owner_org_unit_id key in their Qdrant payload; glaze deserializes
    // a missing string field as "" by default, and "" passed to a PostgreSQL
    // UUID parameter (e.g. the reranker's WHERE d.id = $1::uuid) raises
    // ERROR: invalid input syntax for type uuid. std::optional + glaze's
    // skip_null_members write path keeps round-trips faithful: v1 points
    // round-trip as nullopt, v2+ writes the real value. The resync worker
    // (deferred) is the long-term fix; this keeps the system safe in the
    // meantime. Same rationale as section_id / section_heading.
    std::optional<std::string> owner_org_unit_id;
    std::string              chunk_id;
    int                      chunk_index         = 0;
    int                      authority_level     = 50;   // documents.authority_level; default matches V003
    // All org_unit_ids whose members may retrieve this chunk:
    //   owner_org_unit_id + org_units with a resource_grant giving read access.
    // Used as the MatchAny filter in Qdrant queries.
    std::vector<std::string> access_scope_ids;
    std::string              sensitivity_label   = "internal";
    std::string              lifecycle_status    = "draft";
    std::optional<std::string> activated_at;     // ISO 8601
    std::optional<std::string> superseded_at;    // ISO 8601
    std::optional<std::string> section_id;
    std::optional<std::string> section_heading;
    int                      payload_schema_version = kSchemaVersion;
};

// ---------------------------------------------------------------------------
// ChunkCandidate: result of a Qdrant search before the EvidenceGate check.
// ---------------------------------------------------------------------------

struct ChunkCandidate {
    std::string  chunk_id;
    std::string  document_version_id;
    float        score   = 0.0f;
    ChunkPayload payload;
};

// ---------------------------------------------------------------------------
// AllowedCandidate: a candidate that has passed EvidenceGate.
//
// The type distinction enforces the gate at compile time: the Reranker
// accepts only AllowedCandidate, so a raw ChunkCandidate can never be
// forwarded to reranking without going through the gate.
// ---------------------------------------------------------------------------

struct AllowedCandidate {
    std::string  chunk_id;
    std::string  document_version_id;
    float        score   = 0.0f;
    std::string  text;              // hydrated from Postgres
    std::optional<std::string> section_heading;
};

// ---------------------------------------------------------------------------
// QdrantFilter: access-controlled search filter for a Qdrant query.
//
// QdrantFilterBuilder (Iteration 2) translates AccessScope + Principal
// into this struct. Used directly by VectorStorePort::search().
//
// sensitivity_labels is REQUIRED (no default). The retrieval-orchestrator is
// responsible for deriving the set of labels the requesting session may see
// (e.g. guest -> {public, internal}; member -> {public, internal,
// confidential}; clearance-bearing member -> {..., restricted}). Leaving it
// empty short-circuits the search to zero results -- the same fail-closed
// posture as an empty access_scope_ids. V014 is explicit that restricted
// content must never appear in generated answers, and the only way to enforce
// that at the index is to push the label set down as a filter; PG
// re-validation is defense-in-depth, not the primary gate.
// ---------------------------------------------------------------------------

struct QdrantFilter {
    std::string              company_id;
    std::vector<std::string> access_scope_ids;     // MatchAny on payload field
    std::vector<std::string> sensitivity_labels;   // MatchAny on payload field
    std::string              lifecycle_status = "active";
};

// ---------------------------------------------------------------------------
// UpsertPoint: one point to write to the vector index.
// ---------------------------------------------------------------------------

struct UpsertPoint {
    std::string  id;       // uuid_v5(chunk_id + ":" + embed_model_id)
    Embedding    vector;
    ChunkPayload payload;
};

} // namespace wikore::rag
