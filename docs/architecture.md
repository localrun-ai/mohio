# Wikore C++ Application Architecture (Proposal)

Status: **proposal, pre-implementation**. The schema (V001–V013) is ready
to write against; this document describes how the C++ application layer
should be structured so that Wiki + RAG chat is the first pair of products
rather than the only one.

Audience: anyone about to write C++ code in this repo, or anyone reviewing
that code's structure.

Companion documents:
- `docs/review.md` — original schema review request
- `docs/review_feedback.md` — schema review V1 (12 findings)
- `docs/review_feedback_v2.md` — schema review V2 + product perspective + K1–K8 killer-feature candidates

---

## 1. Headline

**Modular monolith API + extracted workers, hexagonal layering inside,
coroutine-driven I/O on Boost.Asio, retrieval as a composable pipeline.**

Designed so that adding a future product (compliance dashboard, Slack bot,
customer API, plugins) is *new transport + new use cases on top of the same
domain + adapters*, never a rewrite.

---

## 2. Process topology

```
┌────────────────────────────────────────────────────────────┐
│  wikore-api          (single C++ binary, scales horizontally)
│    - HTTP/gRPC entry points for wiki, chat, admin, search
│    - All read-path + light writes
│    - Stateless; multiple replicas behind a load balancer
├────────────────────────────────────────────────────────────┤
│  wikore-ingest       (worker: pulls jobs from outbox)
│    - PDF/DOCX/HTML parse, chunk, embed, push to Qdrant
│    - Long-running, GPU-hungry, isolated crash boundary
│    - Horizontally scalable via job-queue partitioning
├────────────────────────────────────────────────────────────┤
│  wikore-scheduler    (cron/timer worker; single replica)
│    - Document review-due sweeps, tombstone GC,
│      cache warmup, eval runs, SCIM resync, scheduled exports
├────────────────────────────────────────────────────────────┤
│  wikore-eval         (CLI for now; later a worker)
│    - Replays golden questions; writes to eval_runs / eval_grades
└────────────────────────────────────────────────────────────┘
```

**Why modular monolith and not microservices:**

- A real microservice mesh is operational pain that pays off only at scale.
  Wikore's first 50 customers do not need it.
- A single-binary monolith hits a feature-velocity ceiling around the time
  ingest starts to dominate CPU. The split above pre-empts that.
- The shared library codebase has multiple `main()` entry points. Extracting
  any single module to its own service later is *moving an entry point*, not
  rewriting the module.

**Why workers are separate from the API:**

- Ingest is bursty and GPU-bound; the API is steady and CPU-bound.
  Co-deploying them means one starves the other.
- Ingest crashing must not take chat down.
- Ingest may run on different hardware (GPU nodes) than the API.

---

## 3. Internal layering (hexagonal / ports and adapters)

```
┌────────────────────────────────────────────────────────────┐
│ Transport               HTTP, gRPC, internal RPC           │
│                         Thin: validate, decode, dispatch.  │
├────────────────────────────────────────────────────────────┤
│ Application             Use cases:                         │
│                         AskQuestion, PublishWikiPage,      │
│                         IngestDocument, GrantAccess,       │
│                         PromoteDocumentVersion, ...        │
│                         One class per use case.            │
├────────────────────────────────────────────────────────────┤
│ Domain                  Pure logic, no I/O:                │
│                         Document, OrgUnit, AccessScope,    │
│                         WikiPageVersion, ChatTurn,         │
│                         RetrievalQuery, Citation           │
├────────────────────────────────────────────────────────────┤
│ Ports (interfaces)      DocumentRepo, AccessResolver,      │
│                         Retriever, Reranker, LlmProvider,  │
│                         Embedder, ChunkStore, EventBus,    │
│                         AuditSink, FeatureFlagSource       │
├────────────────────────────────────────────────────────────┤
│ Adapters                postgres_adapter (libpqxx async),  │
│                         redis_adapter   (boost::redis),    │
│                         qdrant_adapter  (gRPC),            │
│                         openai_llm / ollama_llm / vllm,    │
│                         onnx_embedder / openai_embedder    │
└────────────────────────────────────────────────────────────┘
```

**Hard rules**

1. Domain depends on nothing. No `#include <pqxx/...>` in the domain layer.
2. Adapters implement domain-defined ports. Domain knows the *port*, not the
   adapter.
3. Application orchestrates: it asks ports for things and composes domain
   operations.
4. Transport translates between wire formats and application inputs/outputs.

**Practical payoff:** swapping Qdrant→Weaviate, OpenAI→local-vLLM, or
Postgres→YugabyteDB is *implementing one new adapter*, with zero changes in
the domain and application layers. This is what enterprise customers ask for
in week three of a POC.

---

## 4. Concurrency model

- **C++23 coroutines on Boost.Asio.** Asio's executor is the maturest async
  ecosystem in C++ and supports Postgres (libpqxx async), Redis
  (boost::redis), gRPC (grpc-cpp async), and HTTP (Beast).
- **One executor pool, threads = cores.** Never block on I/O. Every Postgres,
  Redis, Qdrant, and LLM call is `co_await`-able.
- **Per-request coroutine** owns a `RequestContext&` (tenant, principal,
  scope, trace span, deadline, clock). Never stored, never put in
  thread-local storage, always passed.
- **Cancellation propagates.** Request deadline expired → all downstream
  coroutines cancel cleanly via `asio::cancellation_signal`.

**Things to NOT do:**

- Don't roll your own coroutine runtime. Use Asio.
- Don't mix std::thread with asio's executor for request handling.
- Don't put request-scoped state in TLS or globals.
- Don't `co_await` a synchronous blocking call; wrap it explicitly with
  `asio::post_to_other_pool(...)` if you must.

---

## 5. Cross-cutting types (foundation; build these first)

```cpp
struct Tenant         { uuid company_id; };

struct Principal      { uuid user_id;
                        bool is_admin;
                        std::optional<time_point> deactivated_at; };

struct AccessScope    { std::vector<uuid> org_unit_ids;
                        time_point         cache_until; };

struct RequestContext {
    Tenant                    tenant;
    Principal                 principal;
    AccessScope               scope;
    TraceSpan                 span;
    SteadyClock::time_point   deadline;
    SteadyClock&              clock;        // injectable for tests
    // Not copyable, not movable. Passed by const& everywhere.
};

template <class T>
using Result = std::expected<T, Error>;     // C++23 std::expected
```

**Discipline:**

- Every service method takes `const RequestContext&` as the first argument.
  No "default tenant," no global "current user."
- `Result<T>` for expected business failures (not found, conflict, denied).
- Exceptions reserved for programmer bugs (invariant violation, null
  dereference, OOM).
- Domain types are immutable value types. Adapters produce them, application
  consumes them, transport serialises them.

---

## 6. Retrieval as a composable pipeline

```cpp
class RetrievalStage {
public:
    virtual ~RetrievalStage() = default;
    virtual asio::awaitable<RetrievalResult>
    run(const RequestContext&, RetrievalQuery, RetrievalResult prev) = 0;
};
```

**Default pipeline (built from per-tenant config):**

```
CuratedAnswerStage         (K4 hook; no-op until curated_answers ships)
  → AccessFilterStage      (builds Qdrant payload filter from scope)
  → SensitivityFilterStage (K1 hook; drops docs above principal's clearance)
  → VectorRetrievalStage   (Qdrant top-k)
  → SectionExpansionStage  (K5 hook; pulls parent/sibling chunks)
  → BM25Stage              (optional, post-MVP)
  → RerankerStage          (cross-encoder, optional, GPU-backed)
  → ContextBuilderStage    (assembles LLM prompt + citation list)
```

**Why a pipeline:**

- Each stage is one class. New retrieval feature = one new class.
- Tenant-specific policy = different stage ordering, no code changes.
- Stages are individually testable (mock the previous `RetrievalResult`).
- A/B testing a new ranker = two pipelines built from two configs, deciding
  the winner via the eval harness.

**Stage interface invariants:**

- A stage MUST honour `RequestContext.deadline` (call `expired()` before
  expensive work).
- A stage MUST NOT widen the access scope beyond what `AccessFilterStage`
  produced. Defence in depth.
- A stage that needs to short-circuit (e.g. CuratedAnswerStage hit) returns
  `RetrievalResult{.terminal = true}`; the orchestrator stops there.

---

## 7. Extension model for "more products later"

The architectural commitment: **every new product is new transport + new
use cases on top of the same domain + adapters. Never a rewrite.**

| Future product | What it actually costs |
|---|---|
| Compliance dashboard | New HTTP handlers + new use cases. Reuses domain + access resolver + audit-log adapter. |
| Slack / Teams bot | New transport adapter (Slack events API). Reuses ChatService. |
| Customer-facing API | New auth adapter (PAT scopes) + scoped `Principal`. Reuses everything. |
| Curated-answer admin app | New use cases + one new retrieval stage. |
| Customer plugins | Out-of-process WASM workers via wasmtime, talking gRPC to the API. New plugin-host service, no domain changes. |
| Mobile app | Same gRPC API. |
| Multi-region | DB-side concern; app config picks the region adapter. |
| In-context analytics ("which chunks does Finance reference most?") | New read model materialised by `wikore-scheduler`. |

If a new product wants to change the **domain**, that's a real architectural
decision and warrants its own design doc. New transport or new use cases on
top of existing domain are routine.

---

## 8. Concrete library choices

| Concern | Choice | Why |
|---|---|---|
| HTTP server | **Drogon** for MVP, **Beast** if we outgrow it | Drogon ships fastest with built-in JSON + routing; Beast is the long-term correct asio-native answer |
| gRPC | grpc-cpp async | worker↔api and internal calls |
| Postgres | **sqlpp23** for app code + **libpqxx** async for hot retrieval path | sqlpp23 catches whole classes of bugs at compile time; libpqxx for queries where compile time dominates |
| Redis | **boost::redis** | first-class coroutine support; native Asio integration |
| Qdrant | gRPC client generated from .proto | avoid REST in the hot path |
| JSON | **glaze** | reflection-based, 5–10× faster than nlohmann |
| Config | **toml++** | typed, simple, reload-friendly |
| Logging | **spdlog** with JSON sink | OTel logs later via opentelemetry-cpp |
| Tracing | **opentelemetry-cpp** | bake in from line one, not retrofitted |
| Metrics | Prometheus exposition via prometheus-cpp | scrape-friendly, no push needed |
| Build | **CMake + Conan** | Bazel is overkill for one language |
| Unit tests | **doctest** | header-only, fast compile |
| Integration tests | docker-compose harness (Postgres + Redis + Qdrant + mock LLM) | same shape as `db/smoke_test.sh` |
| Format / lint | clang-format, clang-tidy | enforce in CI |
| Coroutine runtime | **Boost.Asio** | maturest async ecosystem in C++ |
| LLM SDKs | hand-rolled HTTP/JSON adapters | every "official" C++ SDK is half-baked; the surface area is small |
| Embedding (local) | **onnxruntime** | runs sentence-transformers ONNX exports on CPU/GPU/Metal |
| WASM plugin host (future) | **wasmtime-cpp** | when/if K-plugins ship |

---

## 9. Repository layout (proposed)

```
wikore/
├── db/                            # already exists
│   ├── migrations/                # V001..V013
│   └── smoke_test.sh
├── docs/                          # already exists
│   ├── review.md
│   ├── review_feedback.md
│   ├── review_feedback_v2.md
│   ├── architecture.md            # this file
│   └── feedback.md
├── src/
│   ├── domain/                    # pure types, no I/O dependencies
│   │   ├── tenant.hpp
│   │   ├── access_scope.hpp
│   │   ├── document.hpp
│   │   ├── wiki_page_version.hpp
│   │   ├── chat_turn.hpp
│   │   ├── retrieval/             # RetrievalQuery, RetrievalResult, Citation
│   │   └── ports/                 # interfaces only
│   ├── application/
│   │   ├── ask_question.{hpp,cpp}
│   │   ├── publish_wiki_page.{hpp,cpp}
│   │   ├── ingest_document.{hpp,cpp}
│   │   ├── grant_access.{hpp,cpp}
│   │   ├── promote_document_version.{hpp,cpp}
│   │   └── ...
│   ├── adapters/
│   │   ├── postgres/              # implements DocumentRepo etc.
│   │   ├── redis/                 # implements caches + reverse-index Sets
│   │   ├── qdrant/                # implements ChunkStore + Retriever
│   │   ├── llm/                   # openai, ollama, vllm
│   │   └── embedder/              # openai, onnx-local
│   ├── transport/
│   │   ├── http/                  # Drogon controllers
│   │   ├── grpc/                  # for internal worker↔api
│   │   └── auth/                  # OIDC token validation
│   ├── infrastructure/
│   │   ├── logging.{hpp,cpp}
│   │   ├── tracing.{hpp,cpp}
│   │   ├── metrics.{hpp,cpp}
│   │   ├── config.{hpp,cpp}
│   │   └── request_context.{hpp,cpp}
│   ├── retrieval/
│   │   ├── pipeline.{hpp,cpp}
│   │   ├── stages/                # one file per stage
│   │   └── README.md
│   └── apps/                      # multiple main()s
│       ├── api/main.cpp
│       ├── ingest/main.cpp
│       ├── scheduler/main.cpp
│       └── eval/main.cpp
├── tests/
│   ├── unit/                      # mirrors src/
│   └── integration/               # docker-compose-backed
├── proto/                         # .proto files for internal gRPC
├── third_party/                   # vendored deps if any
├── CMakeLists.txt
├── conanfile.txt
└── .clang-format, .clang-tidy, .gitignore
```

The strict directional rule (domain → ports → adapters; application → ports
→ domain; transport → application → domain) is enforced by file layout, not
just convention. CI can add a check (e.g. grep for forbidden includes) once
the codebase is established.

---

## 10. Observability and operations

- **Tracing** via opentelemetry-cpp from line one. Spans on:
  - HTTP request boundary
  - Service call boundary
  - DB query
  - Qdrant call
  - LLM call (with prompt + completion length, cost estimate)
- **Logs**: structured JSON via spdlog; one field is `trace_id` so logs and
  traces correlate.
- **Metrics**: Prometheus exposition; cardinality discipline (never label by
  user_id, only by tenant_id or feature).
- **Per-tenant rate limits** at the API boundary. Per-tenant cost accounting
  (LLM tokens, GPU seconds) writes to an `usage_events` table (post-MVP).
- **Health endpoints**: `/healthz` (process alive), `/readyz` (downstreams
  reachable), `/metrics` (Prometheus).
- **Graceful shutdown**: SIGTERM → stop accepting new requests → wait for
  in-flight coroutines up to a configurable drain timeout → exit.

---

## 11. Security posture

- **TLS terminates at the ingress** (envoy, traefik, or cloud LB). Internal
  service-to-service can be mTLS once extracted.
- **OIDC validation**: every request carries a bearer token; validated
  against per-tenant issuer config. No session cookies.
- **Tenant boundary**: the schema already enforces it via composite FKs.
  `RequestContext.tenant` is the *only* source of tenant identity in the
  app; reject requests where the token's tenant claim disagrees with the
  URL path.
- **Audit log**: every privileged action (grant, revoke, promote, archive,
  delete) writes to `audit_log` *in the same transaction* as the state
  change. Use an `AuditSink` port so the application layer can't forget.
- **Secrets** via Vault / SOPS / cloud secrets manager. Never in env vars
  in plain text. Config file references secret IDs, not values.

---

## 12. Test strategy

- **Unit tests** (doctest): cover domain logic and use cases. Adapters get
  unit tests too (using testcontainers).
- **Integration tests**: docker-compose with real Postgres + real Redis +
  real Qdrant + mock LLM. Same shape as `db/smoke_test.sh`. Run on every
  PR.
- **Schema smoke tests** (`db/smoke_test.sh`): already exist; keep them
  Postgres-only so they stay fast.
- **Eval harness** (post-MVP): runs against the eval schema (K7); replays
  golden questions; produces a per-PR delta on retrieval quality.
- **Load tests** (k6 or wrk2): one canonical scenario per surface (chat,
  wiki render, search). Run before each release candidate.
- **Property-based tests** (rapidcheck): for the access-scope resolver and
  retrieval pipeline composition. Closure-table queries especially benefit.

---

## 13. Things to explicitly NOT do

- **Don't roll your own coroutine runtime.** Use Asio.
- **Don't use exceptions for expected errors.** `Result<T>` for business
  failures; exceptions for programmer bugs only.
- **Don't store request-scoped state in TLS or globals.** Pass
  `RequestContext&`.
- **Don't share repository connections across requests.** Use a per-request
  connection from a pool; release on coroutine completion.
- **Don't put schema invariants in code beyond DDL-checked types.** The
  database is the source of truth for composite FKs, CHECKs, and triggers;
  the app is dumb about them and should fail loudly if they reject a write.
- **Don't ORM everything.** sqlpp23 for typed SQL; raw queries for the hot
  retrieval path; no Hibernate-style auto-loading of relations.
- **Don't write your own auth.** Front-door via Keycloak / Authentik /
  cloud OIDC; the app just validates tokens.
- **Don't add a service mesh until you have at least two extracted
  services.** YAGNI.
- **Don't optimise prematurely.** Wikore's first bottleneck will be LLM
  latency, not C++ overhead. Profile before you tune.

---

## 14. Decision points worth a call before line 1 of code

These are real forks. Each needs an explicit decision in this doc (or in a
follow-up PR amending it) before C++ work starts.

1. **HTTP framework: Drogon vs Beast vs Crow.**
   - Recommendation: **Drogon for MVP**, plan to migrate if scaling forces
     it. Beast is the long-term correct asio-native answer; Drogon ships
     faster.
   - Cost of migration later: re-write transport layer only (~thin layer
     by design); application/domain untouched.

2. **SQL style: sqlpp23 vs libpqxx-raw.**
   - Recommendation: **sqlpp23 for app code, libpqxx-raw for the
     retrieval hot path.** sqlpp23 's compile-time checking is worth the
     learning curve for the long tail of admin/wiki/chat queries; raw
     pqxx wins on retrieval where every microsecond counts and the SQL
     is small and stable.

3. **In-process vs out-of-process LLM/embedder.**
   - Recommendation: **out-of-process for LLM** (vLLM or Ollama
     sidecar); **in-process for local embedder** via ONNX Runtime.
     Embedding is called per-chunk during ingest and per-query during
     retrieval; the network hop is real overhead.

4. **WASM plugin host now or later.**
   - Recommendation: **defer.** Add a stub `PluginHost` port in the
     domain layer so the future plug-in is *additive*, but do not implement
     wasmtime integration until a real customer asks.

5. **Multi-region split.**
   - Recommendation: **defer until a customer needs it.** Architect for
     it (region tag on `companies`, region-aware adapter selection) but
     do not run multi-region clusters yet.

6. **OpenAPI / gRPC / both for the public API.**
   - Recommendation: **OpenAPI-first** for customer-facing; gRPC reserved
     for internal worker↔api. Customers don't want to ship protobufs.

7. **Single repo vs multi-repo for workers.**
   - Recommendation: **single repo, multiple main()s.** Avoid the
     versioning pain of split repos until at least one worker has a wildly
     different cadence.

---

## 15. Open questions for the team

- Which LLM providers do we commit to in week one? OpenAI + local (Ollama
  or vLLM) is the conservative pair. Adding Anthropic / Bedrock / Vertex
  is one adapter each.
- Embedding model: BGE-large-en-v1.5? E5-mistral? something else? This
  decides the Qdrant vector dimension and the chunking budget.
- Authentication provider: do we ship Keycloak in our reference deploy, or
  require customers to bring their own OIDC?
- Do we ship a default ingest pipeline that handles PDFs out of the box,
  or is that a connector that customers configure?
- What is the minimum acceptable Postgres version? V001..V013 work on
  PG17; do we support older?

These are product/ops calls more than architecture calls. Worth a separate
conversation with stakeholders.

---

— Reviewer / proposer: Claude (Opus 4.7)
