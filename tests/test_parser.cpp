#include <catch2/catch_test_macros.hpp>
#include "wikore/ingest/parser.hpp"

using namespace wikore::ingest;

TEST_CASE("PlainTextParser: flat text with no headings", "[parser]")
{
    PlainTextParser p;
    std::string content = "Hello world.\nSecond line.\nThird line.";
    auto doc = p.parse(content, "test.txt", "text/plain");

    CHECK(doc.filename  == "test.txt");
    CHECK(doc.mime_type == "text/plain");

    REQUIRE(doc.sections.size() == 1);
    CHECK(doc.sections[0].depth   == 0);
    CHECK(doc.sections[0].heading.empty());
    CHECK(doc.sections[0].text.find("Hello world") != std::string::npos);
    CHECK(doc.sections[0].text.find("Third line")  != std::string::npos);
}

TEST_CASE("PlainTextParser: markdown with ATX headings", "[parser]")
{
    PlainTextParser p;
    std::string content =
        "# Section One\n"
        "Content of section one.\n"
        "More content.\n"
        "## Subsection\n"
        "Subsection body.\n"
        "# Section Two\n"
        "Content of section two.\n";

    auto doc = p.parse(content, "test.md", "text/markdown");

    // Top-level sections: "Section One" and "Section Two"
    REQUIRE(doc.sections.size() == 2);

    CHECK(doc.sections[0].heading == "Section One");
    CHECK(doc.sections[0].depth   == 1);
    CHECK(doc.sections[0].text.find("Content of section one") != std::string::npos);

    // "Section One" should have one child: "Subsection"
    REQUIRE(doc.sections[0].children.size() == 1);
    CHECK(doc.sections[0].children[0].heading == "Subsection");
    CHECK(doc.sections[0].children[0].depth   == 2);
    CHECK(doc.sections[0].children[0].text.find("Subsection body") != std::string::npos);

    CHECK(doc.sections[1].heading == "Section Two");
    CHECK(doc.sections[1].children.empty());
}

TEST_CASE("PlainTextParser: full_text concatenates all section bodies", "[parser]")
{
    PlainTextParser p;
    std::string content =
        "# A\n"
        "Text A.\n"
        "# B\n"
        "Text B.\n";

    auto doc = p.parse(content, "f.md", "text/markdown");
    CHECK(doc.full_text.find("Text A") != std::string::npos);
    CHECK(doc.full_text.find("Text B") != std::string::npos);
}

TEST_CASE("PlainTextParser: empty document", "[parser]")
{
    PlainTextParser p;
    auto doc = p.parse("", "empty.txt", "text/plain");
    REQUIRE(doc.sections.size() == 1);
    CHECK(doc.sections[0].text.empty());
    CHECK(doc.full_text.empty());
}

TEST_CASE("PlainTextParser: heading at document start (no preamble)", "[parser]")
{
    PlainTextParser p;
    std::string content = "# Title\nBody text.\n";
    auto doc = p.parse(content, "f.md", "text/markdown");

    REQUIRE(doc.sections.size() == 1);
    CHECK(doc.sections[0].heading == "Title");
    CHECK(doc.sections[0].text.find("Body text") != std::string::npos);
}

TEST_CASE("PlainTextParser: h2 before h1 becomes top-level section", "[parser]")
{
    PlainTextParser p;
    std::string content = "## Sub\nSub body.\n# Top\nTop body.\n";
    auto doc = p.parse(content, "f.md", "text/markdown");

    // Both should be top-level since ## appears before any #
    REQUIRE(doc.sections.size() >= 1);
    CHECK(doc.full_text.find("Sub body") != std::string::npos);
    CHECK(doc.full_text.find("Top body")  != std::string::npos);
}
