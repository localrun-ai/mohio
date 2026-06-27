#include "wikore/ingest/chunker.hpp"
#include <cctype>
#include <string_view>

namespace wikore::ingest {

std::vector<Chunk>
Chunker::chunk(const ParsedDocument& doc,
               const std::string&    document_version_id,
               const std::string&    company_id) const
{
    std::vector<Chunk> out;
    int idx = 0;

    if (doc.sections.empty()) {
        // Flat document: chunk full_text as a single section with no heading
        split_text(doc.full_text, document_version_id, company_id,
                   idx, std::nullopt, std::nullopt, out);
    } else {
        for (const auto& section : doc.sections)
            chunk_section(section, document_version_id, company_id, idx, out);
    }
    return out;
}

void Chunker::chunk_section(const ParsedSection& section,
                             const std::string&   document_version_id,
                             const std::string&   company_id,
                             int&                 chunk_idx,
                             std::vector<Chunk>&  out) const
{
    std::optional<std::string> sec_id      = section.db_id;
    std::optional<std::string> sec_heading = section.heading.empty()
                                             ? std::nullopt
                                             : std::optional{section.heading};
    if (!section.text.empty())
        split_text(section.text, document_version_id, company_id,
                   chunk_idx, sec_id, sec_heading, out);

    for (const auto& child : section.children)
        chunk_section(child, document_version_id, company_id, chunk_idx, out);
}

void Chunker::split_text(const std::string&              text,
                          const std::string&              document_version_id,
                          const std::string&              company_id,
                          int&                            chunk_idx,
                          const std::optional<std::string>& section_id,
                          const std::optional<std::string>& section_heading,
                          std::vector<Chunk>&             out) const
{
    if (text.empty()) return;

    int pos = 0;
    const int len = static_cast<int>(text.size());

    while (pos < len) {
        // Tentative end of this chunk
        int end = std::min(pos + kMaxChars, len);

        // Extend to end of current word so we don't cut inside a word
        while (end < len && !std::isspace(static_cast<unsigned char>(text[end])))
            ++end;

        std::string chunk_text = text.substr(pos, end - pos);

        // Trim leading/trailing whitespace from the chunk
        size_t first = chunk_text.find_first_not_of(" \t\n\r");
        size_t last  = chunk_text.find_last_not_of(" \t\n\r");
        if (first == std::string::npos) {
            // Entirely whitespace - skip
            pos = end;
            continue;
        }
        chunk_text = chunk_text.substr(first, last - first + 1);

        out.push_back({
            .document_version_id = document_version_id,
            .company_id          = company_id,
            .chunk_index         = chunk_idx++,
            .text                = std::move(chunk_text),
            .section_id          = section_id,
            .section_heading     = section_heading,
        });

        if (end >= len) break;

        // Overlap: step back kOverlap chars from end, then forward to next word start
        int next_pos = std::max(pos + 1, end - kOverlap);
        while (next_pos < end && !std::isspace(static_cast<unsigned char>(text[next_pos])))
            ++next_pos;
        while (next_pos < end && std::isspace(static_cast<unsigned char>(text[next_pos])))
            ++next_pos;

        // Guard: always advance at least one char to prevent infinite loop
        pos = (next_pos > pos) ? next_pos : end;
    }
}

} // namespace wikore::ingest
