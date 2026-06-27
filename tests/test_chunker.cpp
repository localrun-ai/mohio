#include <catch2/catch_test_macros.hpp>
#include "wikore/ingest/chunker.hpp"

using namespace wikore::ingest;

static ParsedDocument flat_doc(std::string text)
{
    ParsedDocument doc;
    doc.filename  = "test.txt";
    doc.mime_type = "text/plain";
    doc.full_text = text;
    return doc;
}

static ParsedDocument sectioned_doc()
{
    ParsedDocument doc;
    doc.filename  = "test.md";
    doc.mime_type = "text/markdown";

    ParsedSection s1;
    s1.heading = "Introduction";
    s1.depth   = 1;
    s1.text    = "This is the intro section with some text.";
    s1.db_id   = "sec-1";

    ParsedSection s2;
    s2.heading = "Details";
    s2.depth   = 1;
    s2.text    = "This is the details section.";
    s2.db_id   = "sec-2";

    doc.sections = {s1, s2};
    doc.full_text = s1.text + "\n" + s2.text;
    return doc;
}

TEST_CASE("Chunker: empty document produces no chunks", "[chunker]")
{
    Chunker c;
    auto chunks = c.chunk(flat_doc(""), "v1", "co1");
    REQUIRE(chunks.empty());
}

TEST_CASE("Chunker: short text fits in single chunk", "[chunker]")
{
    Chunker c;
    std::string text = "Hello world this is a short text.";
    auto chunks = c.chunk(flat_doc(text), "v1", "co1");
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0].chunk_index == 0);
    CHECK(chunks[0].document_version_id == "v1");
    CHECK(chunks[0].company_id == "co1");
    CHECK(chunks[0].text == "Hello world this is a short text.");
    CHECK(!chunks[0].section_id.has_value());
}

TEST_CASE("Chunker: long text splits into multiple chunks with overlap", "[chunker]")
{
    Chunker c;
    // Build text that is definitely > 600 chars
    std::string text;
    for (int i = 0; i < 30; ++i)
        text += "This is sentence number " + std::to_string(i) + " with some padding. ";

    auto chunks = c.chunk(flat_doc(text), "v1", "co1");
    REQUIRE(chunks.size() > 1);

    // Indices are consecutive
    for (size_t i = 0; i < chunks.size(); ++i)
        CHECK(chunks[i].chunk_index == static_cast<int>(i));

    // Each chunk is within the size limit (may exceed slightly for word-boundary)
    for (const auto& ch : chunks)
        CHECK(ch.text.size() <= Chunker::kMaxChars + 80); // word boundary slack

    // Overlap: the last kOverlap chars of chunk N should appear near the start of chunk N+1
    for (size_t i = 0; i + 1 < chunks.size(); ++i) {
        const auto& prev = chunks[i].text;
        const auto& next = chunks[i + 1].text;
        // The tail of prev should appear somewhere at the start of next
        if (prev.size() >= static_cast<size_t>(Chunker::kOverlap)) {
            std::string tail = prev.substr(prev.size() - Chunker::kOverlap / 2);
            CHECK(next.find(tail) != std::string::npos);
        }
    }
}

TEST_CASE("Chunker: sectioned document attaches section metadata", "[chunker]")
{
    Chunker c;
    auto doc    = sectioned_doc();
    auto chunks = c.chunk(doc, "v1", "co1");

    REQUIRE(chunks.size() >= 2);

    // First chunk should come from section 1
    CHECK(chunks[0].section_id      == std::optional<std::string>{"sec-1"});
    CHECK(chunks[0].section_heading == std::optional<std::string>{"Introduction"});

    // Last chunk should come from section 2
    CHECK(chunks.back().section_id      == std::optional<std::string>{"sec-2"});
    CHECK(chunks.back().section_heading == std::optional<std::string>{"Details"});
}

TEST_CASE("Chunker: chunk indices are globally sequential across sections", "[chunker]")
{
    Chunker c;
    auto doc    = sectioned_doc();
    auto chunks = c.chunk(doc, "v1", "co1");

    for (size_t i = 0; i < chunks.size(); ++i)
        CHECK(chunks[i].chunk_index == static_cast<int>(i));
}

TEST_CASE("Chunker: whitespace-only text produces no chunks", "[chunker]")
{
    Chunker c;
    auto chunks = c.chunk(flat_doc("   \n\n   "), "v1", "co1");
    REQUIRE(chunks.empty());
}
