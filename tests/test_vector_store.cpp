#include <catch2/catch_test_macros.hpp>
#include "wikore/rag/vector_store.hpp"
#include "wikore/rag/embedder.hpp"
#include <drogon/utils/coroutine.h>

using namespace wikore::rag;

static UpsertPoint make_point(std::string id,
                               std::string company_id,
                               std::string version_id,
                               std::string chunk_id,
                               std::string lifecycle = "active",
                               std::vector<std::string> scopes = {"org-1"})
{
    ChunkPayload payload{};
    payload.company_id           = company_id;
    payload.document_id          = "doc-1";
    payload.document_version_id  = version_id;
    payload.chunk_id             = chunk_id;
    payload.access_scope_ids     = std::move(scopes);
    payload.lifecycle_status     = lifecycle;

    NullEmbedder emb(4);
    auto vec = drogon::sync_wait(emb.embed(chunk_id));

    return {std::move(id), *vec, std::move(payload)};
}

TEST_CASE("NullVectorStore: starts empty", "[vector_store]")
{
    NullVectorStore vs;
    CHECK(vs.point_count() == 0);
}

TEST_CASE("NullVectorStore: ensure_collection is a no-op", "[vector_store]")
{
    NullVectorStore vs;
    auto r = drogon::sync_wait(vs.ensure_collection(4));
    CHECK(r.has_value());
    CHECK(vs.point_count() == 0);
}

TEST_CASE("NullVectorStore: upsert stores points", "[vector_store]")
{
    NullVectorStore vs;
    std::vector<UpsertPoint> pts = {
        make_point("p1", "co1", "v1", "c1"),
        make_point("p2", "co1", "v1", "c2"),
    };
    auto r = drogon::sync_wait(vs.upsert(pts));
    REQUIRE(r.has_value());
    CHECK(vs.point_count() == 2);
}

TEST_CASE("NullVectorStore: upsert is idempotent by ID", "[vector_store]")
{
    NullVectorStore vs;
    auto pt = make_point("p1", "co1", "v1", "c1");
    drogon::sync_wait(vs.upsert({pt}));
    drogon::sync_wait(vs.upsert({pt})); // same ID
    CHECK(vs.point_count() == 1);
}

TEST_CASE("NullVectorStore: delete_by_version removes matching points", "[vector_store]")
{
    NullVectorStore vs;
    std::vector<UpsertPoint> pts = {
        make_point("p1", "co1", "v1", "c1"),
        make_point("p2", "co1", "v1", "c2"),
        make_point("p3", "co1", "v2", "c3"),
    };
    drogon::sync_wait(vs.upsert(pts));
    REQUIRE(vs.point_count() == 3);

    auto r = drogon::sync_wait(vs.delete_by_version("co1", "v1"));
    REQUIRE(r.has_value());
    CHECK(vs.point_count() == 1); // only p3 remains
}

TEST_CASE("NullVectorStore: search respects company_id filter", "[vector_store]")
{
    NullVectorStore vs;
    drogon::sync_wait(vs.upsert({make_point("p1", "co1", "v1", "c1")}));
    drogon::sync_wait(vs.upsert({make_point("p2", "co2", "v2", "c2")}));

    NullEmbedder emb(4);
    auto q = drogon::sync_wait(emb.embed("query"));

    QdrantFilter f;
    f.company_id       = "co1";
    f.access_scope_ids = {"org-1"};
    f.lifecycle_status = "active";

    auto r = drogon::sync_wait(vs.search(*q, f, 10));
    REQUIRE(r.has_value());
    CHECK(r->size() == 1);
    CHECK((*r)[0].payload.company_id == "co1");
}

TEST_CASE("NullVectorStore: search respects lifecycle_status filter", "[vector_store]")
{
    NullVectorStore vs;
    drogon::sync_wait(vs.upsert({make_point("p1", "co1", "v1", "c1", "active")}));
    drogon::sync_wait(vs.upsert({make_point("p2", "co1", "v2", "c2", "draft")}));

    NullEmbedder emb(4);
    auto q = drogon::sync_wait(emb.embed("query"));

    QdrantFilter f;
    f.company_id       = "co1";
    f.access_scope_ids = {"org-1"};
    f.lifecycle_status = "active";

    auto r = drogon::sync_wait(vs.search(*q, f, 10));
    REQUIRE(r.has_value());
    CHECK(r->size() == 1);
    CHECK((*r)[0].payload.lifecycle_status == "active");
}

TEST_CASE("NullVectorStore: search respects access_scope_ids filter", "[vector_store]")
{
    NullVectorStore vs;
    drogon::sync_wait(vs.upsert({
        make_point("p1", "co1", "v1", "c1", "active", {"org-A"}),
        make_point("p2", "co1", "v2", "c2", "active", {"org-B"}),
    }));

    NullEmbedder emb(4);
    auto q = drogon::sync_wait(emb.embed("query"));

    QdrantFilter f;
    f.company_id       = "co1";
    f.access_scope_ids = {"org-A"};
    f.lifecycle_status = "active";

    auto r = drogon::sync_wait(vs.search(*q, f, 10));
    REQUIRE(r.has_value());
    CHECK(r->size() == 1);
    CHECK((*r)[0].payload.chunk_id == "c1");
}

TEST_CASE("NullVectorStore: search respects limit", "[vector_store]")
{
    NullVectorStore vs;
    std::vector<UpsertPoint> pts;
    for (int i = 0; i < 10; ++i) {
        pts.push_back(make_point(
            "p" + std::to_string(i), "co1",
            "v" + std::to_string(i), "c" + std::to_string(i)));
    }
    drogon::sync_wait(vs.upsert(pts));

    NullEmbedder emb(4);
    auto q = drogon::sync_wait(emb.embed("query"));

    QdrantFilter f;
    f.company_id       = "co1";
    f.access_scope_ids = {"org-1"};
    f.lifecycle_status = "active";

    auto r = drogon::sync_wait(vs.search(*q, f, 3));
    REQUIRE(r.has_value());
    CHECK(r->size() == 3);
}

TEST_CASE("NullVectorStore: search returns results sorted by score", "[vector_store]")
{
    NullVectorStore vs;
    // Create a query and a set of points; the one with the same embedding as
    // the query should rank first (dot product = 1.0 for unit vectors)
    NullEmbedder emb(4);
    const std::string target_text = "the most relevant document";
    auto target_vec = drogon::sync_wait(emb.embed(target_text));

    UpsertPoint best;
    best.id      = "best-point";
    best.vector  = *target_vec;
    best.payload.company_id          = "co1";
    best.payload.document_version_id = "v-best";
    best.payload.chunk_id            = "c-best";
    best.payload.access_scope_ids    = {"org-1"};
    best.payload.lifecycle_status    = "active";

    drogon::sync_wait(vs.upsert({
        best,
        make_point("p1", "co1", "v1", "c1"),
        make_point("p2", "co1", "v2", "c2"),
    }));

    auto q = drogon::sync_wait(emb.embed(target_text));

    QdrantFilter f;
    f.company_id       = "co1";
    f.access_scope_ids = {"org-1"};
    f.lifecycle_status = "active";

    auto r = drogon::sync_wait(vs.search(*q, f, 10));
    REQUIRE(r.has_value());
    REQUIRE(!r->empty());
    CHECK((*r)[0].chunk_id == "c-best");

    // Scores are descending
    for (size_t i = 1; i < r->size(); ++i)
        CHECK((*r)[i].score <= (*r)[i - 1].score);
}

TEST_CASE("NullVectorStore: empty access_scope_ids returns nothing", "[vector_store]")
{
    // Mirrors Qdrant's MatchAny semantics: an empty `any` array matches NO
    // points. A caller without a resolved scope MUST NOT see any evidence.
    // This matches the production QdrantVectorStore behaviour and prevents
    // tests passing only because Null was more permissive than Qdrant.
    NullVectorStore vs;
    drogon::sync_wait(vs.upsert({
        make_point("p1", "co1", "v1", "c1", "active", {"org-A"}),
        make_point("p2", "co1", "v2", "c2", "active", {"org-B"}),
    }));

    NullEmbedder emb(4);
    auto q = drogon::sync_wait(emb.embed("query"));

    QdrantFilter f;
    f.company_id       = "co1";
    f.access_scope_ids = {};   // empty -> match nothing
    f.lifecycle_status = "active";

    auto r = drogon::sync_wait(vs.search(*q, f, 10));
    REQUIRE(r.has_value());
    CHECK(r->empty());
}
