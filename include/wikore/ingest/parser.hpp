#pragma once
#include "wikore/ingest/types.hpp"
#include <memory>
#include <string>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// ParserPort: abstract document parser.
//
// Implementations:
//   PlainTextParser  - plain text and Markdown (detects # headings)
//
// Future: PdfParser, HtmlParser, DocxParser
// ---------------------------------------------------------------------------

class ParserPort {
public:
    virtual ~ParserPort() = default;

    // Parse raw content bytes into a structured ParsedDocument.
    // mime_type is advisory; the parser may inspect content bytes to override.
    // filename is used for ParsedDocument::filename and may guide parsing hints.
    virtual ParsedDocument parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const = 0;
};

// ---------------------------------------------------------------------------
// PlainTextParser: handles text/plain and text/markdown.
//
// Heading detection: lines starting with one or more '#' followed by a space
// (Markdown ATX headings). For plain text without any '#' headings the whole
// document is a single section with depth=0 and an empty heading.
// ---------------------------------------------------------------------------

class PlainTextParser : public ParserPort {
public:
    ParsedDocument parse(const std::string& content,
                         const std::string& filename,
                         const std::string& mime_type) const override;
};

} // namespace wikore::ingest
