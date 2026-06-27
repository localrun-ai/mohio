#pragma once
#include "wikore/domain/types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// ParsedSection: one heading + its text body from a parsed document.
//
// Section hierarchy (K5): sections can have children (subsections).
// The chunker respects section boundaries - a chunk does not span sections.
// ---------------------------------------------------------------------------

struct ParsedSection {
    std::string                    heading;      // e.g. "2.3 Data Retention"
    int                            depth = 0;    // heading level: 0=doc, 1=h1, 2=h2, ...
    std::string                    text;         // body text under this heading
    std::vector<ParsedSection>     children;
    std::optional<std::string>     db_id;        // populated after writing to document_sections
};

// ---------------------------------------------------------------------------
// ParsedDocument: structured output of a document parser.
// ---------------------------------------------------------------------------

struct ParsedDocument {
    std::string                filename;
    std::string                mime_type;   // "text/plain", "text/html", etc.
    std::vector<ParsedSection> sections;   // top-level sections (may be empty for flat docs)
    std::string                full_text;  // concatenation of all body text
};

// ---------------------------------------------------------------------------
// Chunk: one unit of text to be embedded and stored.
//
// A chunk is always associated with a document_version and optionally with
// a section. chunk_index is the 0-based position within the version.
// ---------------------------------------------------------------------------

struct Chunk {
    std::string              document_version_id;
    std::string              company_id;
    int                      chunk_index = 0;
    std::string              text;
    std::optional<std::string> section_id;      // FK to document_sections.id
    std::optional<std::string> section_heading;
    // Populated after DB write (idempotent upsert by unique index):
    std::optional<std::string> db_id;           // document_chunks.id
};

// ---------------------------------------------------------------------------
// IngestJob: the message read from lr:ingest:q:{company_id}.
//
// Written by the API layer when a document_version transitions to 'pending'.
// The ingest worker consumes these and drives the ingest pipeline.
// ---------------------------------------------------------------------------

struct IngestJob {
    std::string company_id;
    std::string document_id;
    std::string document_version_id;
    std::string file_path;           // absolute path to the source file on shared storage
    std::string embed_model_id;      // e.g. "bge-m3" - determines Qdrant point ID namespace
    int         priority = 0;        // higher = sooner
};

// ---------------------------------------------------------------------------
// IngestProgress: written back to document_versions during ingest.
// ---------------------------------------------------------------------------

// Mirrors V003's `document_versions.ingest_status` CHECK
// (CHECK (ingest_status IN ('pending','processing','done','error'))).
// Adding a new value here REQUIRES extending the CHECK first.
enum class IngestStatus {
    pending,
    processing,
    done,
    error,
};

std::string_view to_string(IngestStatus s);
std::optional<IngestStatus> ingest_status_from_string(std::string_view s);

} // namespace wikore::ingest
