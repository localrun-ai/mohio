#include "wikore/ingest/parser.hpp"
#include <functional>
#include <sstream>
#include <string_view>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// PlainTextParser
//
// Detects ATX-style Markdown headings (lines beginning with 1-6 '#' chars
// followed by a space). Every heading starts a new section; text lines between
// headings are concatenated (newline-separated) into that section's body.
//
// Documents with no headings are treated as a single flat section (depth=0,
// empty heading) whose text is the full document.
// ---------------------------------------------------------------------------

ParsedDocument
PlainTextParser::parse(const std::string& content,
                        const std::string& filename,
                        const std::string& mime_type) const
{
    ParsedDocument doc;
    doc.filename  = filename;
    doc.mime_type = mime_type;

    std::istringstream stream(content);
    std::string        line;

    // Working state: current heading depth and the section being assembled
    struct PendingSection {
        std::string heading;
        int         depth = 0;
        std::string text;
    };

    std::vector<PendingSection> pending; // one per top-level + nested heading in order

    auto flush_to_doc = [&]() {
        // Convert flat pending list into a section tree.
        // We stack sections by depth, nesting shallower ones into deeper ones.
        std::vector<ParsedSection*> stack; // stack[i] = last section at depth i+1

        for (auto& p : pending) {
            ParsedSection sec;
            sec.heading = p.heading;
            sec.depth   = p.depth;
            sec.text    = p.text;

            if (p.depth == 0 || stack.empty()) {
                doc.sections.push_back(std::move(sec));
                stack.clear();
                stack.push_back(&doc.sections.back());
            } else {
                // Pop until we find an ancestor with strictly smaller depth.
                while (!stack.empty() && stack.back()->depth >= p.depth)
                    stack.pop_back();

                if (stack.empty()) {
                    // Same or higher level than every ancestor: top-level section.
                    doc.sections.push_back(std::move(sec));
                    stack.push_back(&doc.sections.back());
                } else {
                    stack.back()->children.push_back(std::move(sec));
                    stack.push_back(&stack.back()->children.back());
                }
            }
        }
    };

    PendingSection current{};
    bool           has_headings = false;

    while (std::getline(stream, line)) {
        // Detect ATX heading: 1-6 '#' then a space
        int hashes = 0;
        while (hashes < static_cast<int>(line.size()) && line[hashes] == '#')
            ++hashes;

        if (hashes >= 1 && hashes <= 6
            && hashes < static_cast<int>(line.size())
            && line[hashes] == ' ')
        {
            // Save previous section
            if (has_headings || !current.text.empty())
                pending.push_back(current);

            has_headings    = true;
            current.heading = line.substr(hashes + 1);
            current.depth   = hashes;
            current.text    = {};
        } else {
            if (!current.text.empty())
                current.text += '\n';
            current.text += line;
        }
    }

    // Flush final section
    pending.push_back(current);

    if (!has_headings) {
        // No headings: single flat section
        ParsedSection flat;
        flat.heading = {};
        flat.depth   = 0;
        flat.text    = current.text;
        doc.sections.push_back(std::move(flat));
    } else {
        flush_to_doc();
    }

    // Build full_text as concatenation of all section texts (depth-first)
    std::function<void(const ParsedSection&)> append_text =
        [&](const ParsedSection& s) {
            if (!doc.full_text.empty() && !s.text.empty())
                doc.full_text += '\n';
            doc.full_text += s.text;
            for (const auto& c : s.children)
                append_text(c);
        };
    for (const auto& s : doc.sections)
        append_text(s);

    return doc;
}

} // namespace wikore::ingest
