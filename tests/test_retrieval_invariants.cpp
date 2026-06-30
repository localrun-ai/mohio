#include <catch2/catch_test_macros.hpp>
#include "wikore/access.hpp"
#include "wikore/ingest/document_repo.hpp"
#include "wikore/db.hpp"
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <algorithm>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Iteration 2 / section-5 item 1: G1 subset property test.
//
// G1 (gate authority): a chunk reaches the reranker only if a single
// authoritative Postgres query says the reader may see it. Under option (a)
// the gate resolves the resource axis live from resource_grants, so the gate
// output must equal the authoritative truth for ANY grant/membership/tree
// configuration. We assert that across randomized configurations:
//
//   gate(reader, candidates)  ==  oracle(reader, candidates)
//
// where:
//   * gate   = the canonical SET-BASED G1 query (docs/iteration_2_design.md
//              section 0). It is the code under test.
//   * oracle = an INDEPENDENT formulation built from two pre-existing,
//              separately-written resolvers:
//                - PostgresDocumentRepo::fetch_access_scopes(doc) -> the
//                  resource-side expansion (which OUs can see the doc), and
//                - AccessService::effective_read_orgs(reader)     -> the
//                  reader-side scope.
//              A doc is visible iff those two sets intersect.
//
// Equality (not just subset) is the right assertion here: because the gate
// reads live Postgres, there is no overlay lag at the gate layer, so safety
// (gate is a subset of truth = no leak) and liveness (gate equals truth once
// consistent) collapse to a single equality check per configuration. The
// W1/W2/W3 prefilter-staleness interleaving test layers on top of this once
// EvidenceGate + the section-2 overlay exist; this test pins the boundary.
//
// History: the first cut of the section-0 query matched principal_id against
// reader_grant_keys (reader_scope + ancestors) for ALL grants, which silently
// ignored principal_applies_to and over-granted self_only-principal grants to
// readers sitting in a descendant of the principal. This test was written to
// catch exactly that class of divergence between the reader-side inversion and
// the resource-side truth.
// ---------------------------------------------------------------------------

namespace {

constexpr auto CO = "61c00000-0000-0000-0000-0000000000a1"; // "g1 co"

bool db_available() { return std::getenv("DATABASE_URL") != nullptr; }

template<typename... Args>
drogon::orm::Result exec_sync(drogon::orm::DbClientPtr db, std::string sql, Args... args)
{
    return drogon::sync_wait(
        [db, sql = std::move(sql), ...args = std::move(args)]()
        -> drogon::Task<drogon::orm::Result> {
            co_return co_await db->execSqlCoro(sql, args...);
        }());
}

// Postgres uuid[] / text[] literal from a vector.
std::string pg_array(const std::vector<std::string>& v)
{
    std::string out = "{";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += '"';
        out += v[i];
        out += '"';
    }
    out += '}';
    return out;
}

// The canonical SET-BASED G1 gate query (docs/iteration_2_design.md s0).
// $1 company, $2 lifecycle[], $3 sensitivity[], $4 reader_scope uuid[],
// $5 candidate chunk-id uuid[]. Returns the visible doc ids.
constexpr auto kG1Sql = R"(
    WITH reader_scope(ou_id) AS (
        SELECT DISTINCT x FROM unnest($4::uuid[]) AS x
    ),
    reader_grant_keys AS (
        SELECT ou_id FROM reader_scope
        UNION
        SELECT c.ancestor_id
        FROM   org_unit_closure c
        WHERE  c.company_id = $1::uuid
          AND  c.descendant_id IN (SELECT ou_id FROM reader_scope)
    ),
    cand_docs AS (
        SELECT DISTINCT d.id AS doc_id, d.owner_org_unit_id AS owner
        FROM   document_chunks   dc
        JOIN   document_versions dv ON dv.id = dc.document_version_id
        JOIN   documents         d  ON d.id  = dv.document_id
        WHERE  dc.company_id        = $1::uuid
          AND  dc.id                = ANY($5::uuid[])
          AND  dv.lifecycle_status  = ANY($2)
          AND  dv.sensitivity_label = ANY($3)
    ),
    visible AS (
        -- arm 1: reader is in the document's owner scope
        SELECT doc_id FROM cand_docs
        WHERE  owner IN (SELECT ou_id FROM reader_scope)
      UNION
        -- arm 2: a document-level read grant whose principal reaches reader
        SELECT cd.doc_id
        FROM   cand_docs cd
        JOIN   resource_grants rg
            ON rg.company_id    = $1::uuid
           AND rg.resource_type = 'document'
           AND rg.resource_id   = cd.doc_id
           AND rg.permission IN ('read','write','admin')
           AND (rg.expires_at IS NULL OR rg.expires_at > now())
        WHERE  (rg.principal_applies_to = 'self_only'
                AND rg.principal_id IN (SELECT ou_id FROM reader_scope))
           OR  (rg.principal_applies_to = 'self_and_descendants'
                AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys))
      UNION
        -- arm 3: an org-unit read grant on an ancestor of the owner whose
        --        principal reaches reader (resource subtree via closure)
        SELECT cd.doc_id
        FROM   cand_docs cd
        JOIN   org_unit_closure rc
            ON rc.company_id    = $1::uuid
           AND rc.descendant_id = cd.owner
        JOIN   resource_grants rg
            ON rg.company_id    = $1::uuid
           AND rg.resource_type = 'org_unit'
           AND rg.resource_id   = rc.ancestor_id
           AND rg.permission IN ('read','write','admin')
           AND (rg.expires_at IS NULL OR rg.expires_at > now())
           AND (rg.resource_applies_to = 'self_and_descendants'
                OR (rg.resource_applies_to = 'self_only' AND rc.depth = 0))
        WHERE  (rg.principal_applies_to = 'self_only'
                AND rg.principal_id IN (SELECT ou_id FROM reader_scope))
           OR  (rg.principal_applies_to = 'self_and_descendants'
                AND rg.principal_id IN (SELECT ou_id FROM reader_grant_keys))
    )
    SELECT doc_id::text FROM visible
)";

struct Trial {
    std::vector<std::string> ous;      // all org_unit ids (incl root at [0])
    std::vector<std::string> docs;     // document ids
    std::vector<std::string> chunks;   // chunk ids (one per doc)
    std::vector<std::string> users;    // user ids
};

// Build a random tenant: tree, documents (+active version +1 chunk each),
// grants, memberships. Returns the ids needed to drive the assertions.
Trial build_random_trial(drogon::orm::DbClientPtr db, std::mt19937& rng)
{
    exec_sync(db, "DELETE FROM companies WHERE id=$1::uuid", std::string(CO));
    exec_sync(db,
        "INSERT INTO companies (id, name, slug) VALUES ($1::uuid, 'G1', 'g1')",
        std::string(CO));

    Trial t;
    auto root = exec_sync(db,
        "SELECT id FROM org_units WHERE company_id=$1::uuid AND type='root'",
        std::string(CO));
    t.ous.push_back(std::string(root[0]["id"].c_str()));

    // Random tree: each new OU hangs off a random existing OU.
    const int n_ou = 6 + (rng() % 8);              // 6..13 OUs
    for (int i = 0; i < n_ou; ++i) {
        const auto& parent = t.ous[rng() % t.ous.size()];
        auto r = exec_sync(db,
            "INSERT INTO org_units (company_id, parent_id, type, slug, name) "
            "VALUES ($1::uuid, $2::uuid, 'team', $3, $3) RETURNING id",
            std::string(CO), parent, std::string("ou") + std::to_string(i));
        t.ous.push_back(std::string(r[0]["id"].c_str()));
    }

    // Documents: each owned by a random OU, one active version, one chunk.
    const int n_doc = 3 + (rng() % 4);             // 3..6 docs
    for (int i = 0; i < n_doc; ++i) {
        const auto& owner = t.ous[rng() % t.ous.size()];
        auto d = exec_sync(db,
            "INSERT INTO documents (company_id, owner_org_unit_id, filename) "
            "VALUES ($1::uuid, $2::uuid, $3) RETURNING id",
            std::string(CO), owner, std::string("d") + std::to_string(i) + ".txt");
        const auto doc_id = std::string(d[0]["id"].c_str());
        t.docs.push_back(doc_id);
        auto v = exec_sync(db,
            "INSERT INTO document_versions "
            "(company_id, document_id, version_no, source_hash, ingest_status, "
            " completed_at, activated_at, chunk_count, lifecycle_status) "
            "VALUES ($1::uuid, $2::uuid, 1, 'h', 'done', now(), now(), 1, 'active') "
            "RETURNING id",
            std::string(CO), doc_id);
        const auto ver_id = std::string(v[0]["id"].c_str());
        auto c = exec_sync(db,
            "INSERT INTO document_chunks "
            "(company_id, document_version_id, chunk_index, content, content_hash, "
            " qdrant_prefilter_scope_ids) "
            "VALUES ($1::uuid, $2::uuid, 0, 'x', $3, '{}') RETURNING id",
            std::string(CO), ver_id, std::string("h") + doc_id);
        t.chunks.push_back(std::string(c[0]["id"].c_str()));
    }

    // Grants: random resource (a document or an org_unit), random principal OU,
    // random applies_to on both axes. permission read.
    const char* applies[] = {"self_only", "self_and_descendants"};
    const int n_grant = 4 + (rng() % 5);           // 4..8 grants
    for (int i = 0; i < n_grant; ++i) {
        const auto& principal = t.ous[rng() % t.ous.size()];
        const char* p_applies = applies[rng() % 2];
        if (rng() % 2 == 0 && !t.docs.empty()) {
            // document-resource grant (resource_applies_to forced self_only)
            const auto& res = t.docs[rng() % t.docs.size()];
            exec_sync(db,
                "INSERT INTO resource_grants "
                "(company_id, resource_type, resource_id, resource_applies_to, "
                " principal_type, principal_id, principal_applies_to, permission) "
                "VALUES ($1::uuid,'document',$2::uuid,'self_only','org_unit',$3::uuid,$4,'read') "
                "ON CONFLICT DO NOTHING",
                std::string(CO), res, principal, std::string(p_applies));
        } else {
            const auto& res = t.ous[rng() % t.ous.size()];
            const char* r_applies = applies[rng() % 2];
            exec_sync(db,
                "INSERT INTO resource_grants "
                "(company_id, resource_type, resource_id, resource_applies_to, "
                " principal_type, principal_id, principal_applies_to, permission) "
                "VALUES ($1::uuid,'org_unit',$2::uuid,$3,'org_unit',$4::uuid,$5,'read') "
                "ON CONFLICT DO NOTHING",
                std::string(CO), res, std::string(r_applies),
                principal, std::string(p_applies));
        }
    }

    // Users with 1-2 memberships each.
    const int n_user = 3 + (rng() % 3);            // 3..5 users
    for (int i = 0; i < n_user; ++i) {
        auto u = exec_sync(db,
            "INSERT INTO users (company_id, external_sub, email) "
            "VALUES ($1::uuid, $2, $3) RETURNING id",
            std::string(CO), std::string("sub") + std::to_string(i),
            std::string("u") + std::to_string(i) + "@g1.test");
        const auto uid = std::string(u[0]["id"].c_str());
        t.users.push_back(uid);
        const int n_mem = 1 + (rng() % 2);
        for (int m = 0; m < n_mem; ++m) {
            const auto& ou = t.ous[rng() % t.ous.size()];
            exec_sync(db,
                "INSERT INTO memberships (company_id, user_id, org_unit_id, role, applies_to) "
                "VALUES ($1::uuid, $2::uuid, $3::uuid, 'viewer', $4) ON CONFLICT DO NOTHING",
                std::string(CO), uid, ou, std::string(applies[rng() % 2]));
        }
    }
    return t;
}

} // namespace

TEST_CASE("G1 property: gate output equals authoritative PG truth across random configs",
          "[integration][retrieval][g1]")
{
    if (!db_available()) SKIP("DATABASE_URL not set");
    auto db = wikore::Db::get();
    wikore::AccessService          access(db);
    wikore::ingest::PostgresDocumentRepo repo(db);

    const std::vector<std::string> lifecycles{"active"};
    const std::vector<std::string> sensitivities{"public", "internal", "confidential", "restricted"};

    constexpr unsigned kTrials = 40;
    for (unsigned seed = 1; seed <= kTrials; ++seed) {
        std::mt19937 rng(seed);
        Trial t = build_random_trial(db, rng);
        const auto& root = t.ous[0];

        // Candidate set = every chunk in the tenant.
        const auto cand_lit = pg_array(t.chunks);

        for (const auto& user : t.users) {
            // ---- reader scope (company-wide: query scope = root) ----
            auto reader_scope = drogon::sync_wait(access.effective_read_orgs(CO, user, root));

            // ---- ORACLE: doc visible iff fetch_access_scopes(doc) ∩ reader_scope ----
            std::set<std::string> scope_set(reader_scope.begin(), reader_scope.end());
            std::set<std::string> oracle_visible;
            for (const auto& doc : t.docs) {
                auto res = drogon::sync_wait(repo.fetch_access_scopes(CO, doc));
                REQUIRE(res.has_value());
                for (const auto& ou : *res) {
                    if (scope_set.count(ou)) { oracle_visible.insert(doc); break; }
                }
            }

            // ---- GATE: the canonical set-based G1 query ----
            auto rows = exec_sync(db, kG1Sql,
                std::string(CO),
                pg_array(lifecycles),
                pg_array(sensitivities),
                pg_array(reader_scope),
                cand_lit);
            std::set<std::string> gate_visible;
            for (const auto& r : rows) gate_visible.insert(std::string(r["doc_id"].c_str()));

            // ---- assert equality (safety + liveness) ----
            INFO("seed=" << seed << " user=" << user
                 << " reader_scope_size=" << reader_scope.size());
            if (gate_visible != oracle_visible) {
                // Surface the divergence direction for debugging.
                for (const auto& d : gate_visible)
                    if (!oracle_visible.count(d))
                        FAIL("OVER-GRANT (leak): gate emitted doc " << d
                             << " not in PG truth (seed=" << seed
                             << " user=" << user << ")");
                for (const auto& d : oracle_visible)
                    if (!gate_visible.count(d))
                        FAIL("under-grant (recall): gate dropped doc " << d
                             << " that PG truth allows (seed=" << seed
                             << " user=" << user << ")");
            }
            CHECK(gate_visible == oracle_visible);
        }
    }
}
