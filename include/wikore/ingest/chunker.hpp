#pragma once
#include "wikore/ingest/types.hpp"
#include <vector>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// Chunker: splits a ParsedDocument into Chunks for embedding.
//
// - Target chunk size: ~600 characters (soft limit; always ends on whitespace)
// - Overlap: 100 characters (carried from previous chunk's tail)
// - Section-aware: chunks do not cross section boundaries
// - chunk_index is 0-based across the entire document (not per-section)
// ---------------------------------------------------------------------------

class Chunker {
public:
    // Target character count per chunk (does not count the overlap prefix).
    static constexpr int kMaxChars  = 600;
    // Characters of overlap to carry from the previous chunk.
    static constexpr int kOverlap   = 100;

    // Chunk the entire document. document_version_id and company_id are stamped
    // onto every Chunk. Chunk::db_id is left empty; the repo layer populates it
    // after the DB write.
    std::vector<Chunk> chunk(const ParsedDocument& doc,
                             const std::string& document_version_id,
                             const std::string& company_id) const;

private:
    // Recursively chunk one section and its children.
    // chunk_idx is incremented in-place as chunks are produced.
    void chunk_section(const ParsedSection&    section,
                       const std::string&      document_version_id,
                       const std::string&      company_id,
                       int&                    chunk_idx,
                       std::vector<Chunk>&     out) const;

    // Split a single text block into chunks with overlap. The section_id and
    // heading are attached to each chunk verbatim.
    void split_text(const std::string&              text,
                    const std::string&              document_version_id,
                    const std::string&              company_id,
                    int&                            chunk_idx,
                    const std::optional<std::string>& section_id,
                    const std::optional<std::string>& section_heading,
                    std::vector<Chunk>&             out) const;
};

} // namespace wikore::ingest
