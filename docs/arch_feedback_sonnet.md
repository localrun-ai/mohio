# Architecture Feedback (Sonnet)

Reviewer context: read architecture.md in full against the finalized schema
(V001-V014) and the existing C++ experience from the astraea.cpp project.

Overall verdict: the layering model is correct and the process topology is the
right split. The objections below are mostly about internal contradictions and
gaps that will cause friction on day one of coding.

---

## Hard blocker: Drogon and Boost.Asio are not the same event loop

Section 4 commits to "C++23 coroutines on Boost.Asio. Asio's executor is the
maturest async ecosystem." Section 8 recommends Drogon as the HTTP server.

These are incompatible. Drogon runs on its own event loop (trantor, libuv-like
internally). You cannot `co_await` Drogon futures from Boost.Asio coroutines
without bridging the two executors, which is exactly the kind of impedance
mismatch that produces subtle data races and cancellation bugs.

Options, pick one before line 1:

Option A - Go full Boost.Asio: HTTP via Beast, Redis via boost::redis,
Postgres via libpqxx async, all coroutines are `asio::awaitable<T>`. Consistent
but Beast requires more boilerplate for routing and middleware than Drogon.

Option B - Go full Drogon: HTTP via Drogon controllers, coroutines are
`drogon::Task<T>`, async Postgres via `drogon::orm::Mapper`. The concurrency
section of the architecture must change to reflect Drogon's model, not Asio's.
This is what the astraea.cpp plan originally assumed.

Option C - Beast for the hot HTTP path, Drogon for admin/wiki/scaffolding.
Two event loops in one binary is worse than picking either.

Recommendation: if retrieval latency and streaming SSE are the primary
concerns, stay on Drogon + drogon::Task for MVP (already proven in astraea.cpp)
and revisit Beast when load tests show it is the bottleneck. The architecture
document should stop recommending Asio in Section 4 if Drogon is the actual
choice.

---

## Objection 1: RequestContext.scope is eagerly populated but scope resolution is expensive

Section 5 shows:

```cpp
struct RequestContext {
    AccessScope scope;   // already populated
    ...
};
```

Scope resolution goes: check Redis (cache miss) -> Postgres membership +
resource_grants query -> compute org_unit transitive closure -> write Redis.
For requests that do not need scope (health check, API key create, audit log
read, wiki page metadata fetch), this is wasted work on the hot path.

Suggestion: make scope lazy. Either `std::optional<AccessScope> scope` set only
when `AccessResolver::resolve()` is first called, or a thin wrapper that
resolves on first access. Every use case that needs scope calls the resolver;
those that don't pay nothing.

---

## Objection 2: AuditSink transaction sharing is not described

Section 11: "every privileged action writes to audit_log in the same
transaction as the state change."

The AuditSink is a port. Its adapter writes to Postgres. For the audit write
to be in the same transaction as the main operation, the AuditSink adapter
must receive (or share) the same database connection that the main repository
adapter is using.

The architecture does not describe how transactions are propagated across
adapters. Three common patterns, each with trade-offs:

- Unit-of-Work: a `Transaction` object is created in the application layer and
  passed to both the repo adapter and the AuditSink adapter. Clean but adds a
  `Transaction&` parameter to every use case.
- Implicit ambient transaction: stored in some scoped object on the coroutine
  frame. Fragile if coroutines are moved or if the AuditSink is called after
  the transaction has been released.
- Two-phase: main operation commits, then audit is written in a separate
  transaction. Simple but violates the stated invariant (audit can be lost if
  the process crashes between the two commits).

Pick one and document it. The Unit-of-Work pattern with an explicit
`Transaction&` parameter is the cleanest for a coroutine-per-request model.

---

## Objection 3: Postgres connection held during LLM streaming

Section 13: "Use a per-request connection from a pool; release on coroutine
completion."

A chat request that streams from an LLM takes 10-60 seconds. "Release on
coroutine completion" means the Postgres connection is held open for that
entire duration doing nothing, while the coroutine is blocked in the LLM
streaming loop.

With a 20-connection Postgres pool and any reasonable concurrency, this
exhausts the pool quickly.

The correct pattern for coroutine-based code: acquire a connection for each
DB section, then release it before entering any I/O wait. For a chat request:

```
acquire_pg_conn -> resolve scope -> release
acquire_pg_conn -> fetch retrieval results -> release
[ LLM streaming - no PG connection held ]
acquire_pg_conn -> write chat_turn + audit_log -> release
```

The architecture should state this explicitly, because the naive reading of
"per-request connection" invites the bug.

---

## Objection 4: No Qdrant resync consumer in the process topology

V008 defines `lr:resync:q` (a Redis list) as the queue for Qdrant payload
resyncs when access_scope_ids change. The schema's Redis invalidation model
depends on a background worker consuming this queue.

The process topology in Section 2 lists: api, ingest, scheduler, eval.
None of them is assigned ownership of `lr:resync:q`. The scheduler seems like
the natural home ("tombstone GC" is listed there), but it is not stated.

This matters because the resync worker is the thing that makes permission
changes actually take effect in Qdrant. If it is not assigned, it will not
get built.

---

## Objection 5: Ingest job enqueue has no crash-safety story

When a document is uploaded:
1. API writes `document_versions` row with `ingest_status='pending'`.
2. API pushes job to `lr:ingest:q:{org_unit_id}` (Redis list, V008).
3. wikore-ingest worker pops the job and processes it.

If the API crashes between step 1 (committed) and step 2 (pushed), the
ingest job is lost. The document row sits at `ingest_status='pending'`
forever.

The architecture does not describe a recovery path. Two options:

- Polling fallback: wikore-scheduler periodically queries for rows stuck at
  `ingest_status='pending'` for more than N minutes and re-enqueues them.
  Simple, matches what `wikore-scheduler` is already doing for review-due
  sweeps.
- Transactional outbox: write the job to a `pending_ingest_jobs` table in
  the same transaction as the `document_versions` insert, then a separate
  poller promotes it to Redis. Stronger guarantee but more schema.

For MVP, the polling fallback in wikore-scheduler is fine. It should be
stated in the architecture so it gets built.

---

## Objection 6: src/retrieval/ placement is architecturally ambiguous

The repository layout has `src/retrieval/` at the same level as
`src/domain/`, `src/application/`, and `src/adapters/`. This implies that
retrieval is a separate architectural layer.

But the retrieval pipeline is an application-layer concern: it orchestrates
ports (Retriever, Reranker, Embedder, ChunkStore), uses domain types
(RetrievalQuery, RetrievalResult, Citation), and is called by the
AskQuestion use case. It is not an adapter and it is not pure domain logic.

Two cleaner options:
- Move to `src/application/retrieval/` so it lives next to ask_question.cpp.
- Move the stage interface (`RetrievalStage`) to `src/domain/ports/` and the
  implementations to `src/adapters/retrieval/` or `src/application/retrieval/`.

The current placement will make the include-direction enforcement (Section 9's
"strict directional rule") harder to express in CMake or CI because retrieval
has ambiguous dependencies.

---

## Objection 7: Two SQL libraries is extra complexity for MVP

Section 8 recommends sqlpp23 for app code and libpqxx-raw for the retrieval
hot path. That is two build dependencies, two mental models, two sets of
error-handling patterns, and two code-review idioms.

sqlpp23's compile-time SQL checking is a real benefit for the long tail of
admin/wiki queries. But it requires code generation and a learning curve.
For a team starting from scratch, the first month of velocity will be spent
learning sqlpp23 DSL, not writing business logic.

Suggestion: start with libpqxx (or Drogon ORM if going the Drogon route)
across the board. Add sqlpp23 in a later pass once the query surface is
understood. The decision can be deferred without schema changes; it is purely
a code-layer concern.

---

## Objection 8: vcpkg vs Conan

The astraea.cpp project used vcpkg (manifest mode, vcpkg.json). Conan is
recommended here. Both work; the friction is in switching between them if
any shared toolchain exists.

If this repo will be built on the same machine as astraea.cpp, standardize on
one package manager. vcpkg manifest mode is simpler for a single-language C++
project; Conan's profile system matters more for multi-platform builds.

Not a blocker, but worth a decision before the first CMakeLists.txt.

---

## Minor clarifications

**FeatureFlagSource port (Section 3, ports list)**: the doc lists it but never
describes what it does. The schema already has `companies.settings JSONB` for
per-tenant feature flags. Does FeatureFlagSource just read that? Or does it
imply an external system (LaunchDarkly, Unleash)? For MVP, a thin adapter
over `companies.settings` is sufficient. State this or remove the port from
the list.

**EventBus port (Section 3, ports list)**: similarly listed but undefined.
What events does it publish? To whom? In-process observer pattern? Redis
pub/sub? Kafka? If the only consumers are the Qdrant resync worker and the
audit log, those are already handled by `lr:resync:q` and the AuditSink port
respectively. An EventBus abstraction adds an indirection layer without a
clear owner. Either describe it concretely or remove it from the MVP port list.

**Principal.deactivated_at in RequestContext (Section 5)**: a deactivated user
should fail JWT validation before reaching the application layer (the OIDC
middleware should reject them). If deactivated_at appears in Principal, it
implies the auth layer deliberately lets deactivated users through. Is that
intentional (deactivation is application-layer only, not auth-layer)? If so,
say so explicitly; otherwise remove the field and let auth reject them.

**AccessFilterStage vs pre-processing (Section 6)**: AccessFilterStage
translates the already-resolved AccessScope from RequestContext into a Qdrant
filter. This is serialization, not computation. Calling it a "stage" implies
it participates in the pipeline's data flow like VectorRetrievalStage does,
but it does not produce or filter RetrievalResults - it produces a filter spec.
Consider naming it `QdrantFilterBuilder` and making it a pre-processing step
called before the pipeline runs rather than a stage that can short-circuit.

**Section 9 layout shows V001..V013 but we are now at V014**: minor staleness.

---

## Summary

| # | Severity | Item |
|---|----------|------|
| - | BLOCKER | Drogon vs Boost.Asio: pick one event loop before line 1 |
| 1 | RECOMMENDED | AccessScope in RequestContext should be lazy, not eager |
| 2 | RECOMMENDED | AuditSink transaction sharing pattern must be described |
| 3 | RECOMMENDED | PG connection must be released before LLM streaming, not at coroutine end |
| 4 | RECOMMENDED | Assign Qdrant resync worker (`lr:resync:q` consumer) to a process |
| 5 | RECOMMENDED | Document ingest job crash-recovery (scheduler polling fallback) |
| 6 | OPTIONAL | src/retrieval/ placement is architecturally ambiguous; move to application/ |
| 7 | OPTIONAL | Two SQL libraries is extra friction for MVP; defer sqlpp23 |
| 8 | OPTIONAL | Decide vcpkg vs Conan before first CMakeLists.txt |
| - | MINOR | FeatureFlagSource and EventBus ports need concrete description or removal |
| - | MINOR | Principal.deactivated_at: clarify whether auth layer or app layer owns deactivation |
| - | MINOR | AccessFilterStage is a serializer, not a pipeline stage; rename or relocate |

---

## Library choices critique (Section 8) and decision points (Section 14)

### Are we picking the right tradeoffs for enterprise B2B SaaS on C++23?

Short answer: the stack is mostly sound, but four choices carry disproportionate
cost for the benefit delivered at MVP stage. Two of them (Asio + Drogon, sqlpp23)
are already in the blockers/recommended list above. This section adds sharper
reasoning and covers the items not yet called out.

---

### Section 8 - library by library

**Drogon + Boost.Asio (hard blocker - repeated here in context)**

Already the top item above. The table says "Drogon for MVP, Beast if we outgrow
it" in the HTTP row, and "Boost.Asio" in the coroutine runtime row, as if these
compose. They do not. Picking Drogon means your coroutines are drogon::Task<T>
and your async primitives are trantor-based. Picking Asio means your HTTP is Beast
or cpp-httplib-over-asio. The table needs one row, not two contradictory choices.

Verdict: pick Drogon for MVP. It is faster to be productive on and already
validated in this codebase (astraea.cpp). Remove "Boost.Asio" from Section 4 and
Section 8 coroutine runtime row; replace with "drogon::Task<T> / trantor".

**grpc-cpp for internal worker<->api communication**

At MVP, wikore has four processes: api, ingest, scheduler, eval. They communicate
via:
- Redis queues (ingest job queue, resync queue - already in V008)
- Postgres (document_versions.ingest_status, scheduler sweep queries)
- No real-time RPC needed between them at launch

grpc-cpp adds: protobuf schema files, code generation step in CMake, async gRPC
stubs, bi-directional streaming setup, protobuf versioning discipline. The payoff
(typed contracts, language-agnostic wire format, efficient binary encoding) is
real - but only when two teams evolve two services independently, or when the
services span languages. For four processes in one repo on one machine,
serializing job payloads as JSON into a Redis list is simpler and already
architected in the schema.

Verdict: **drop gRPC for internal communication at MVP**. The existing Redis
queue + Postgres polling pattern is sufficient for ingest<->api and
scheduler<->api communication. Revisit when you extract a worker to a separate
repo or a different language. Keep the .proto directory stubbed in the layout for
when it matters.

**sqlpp23 for app code**

Already in Objection 7. Worth adding the B2B SaaS angle: enterprise customers
often want custom integrations and report queries. Those queries tend to be written
by whoever is on-call, not by the person who designed the sqlpp23 schema files.
A raw libpqxx query is immediately readable and debuggable; a sqlpp23 DSL
expression requires knowing the DSL. For a SaaS company with a small team, the
cognitive load is borne by every future hire.

Verdict: **defer sqlpp23 entirely**. Use Drogon ORM for CRUD-shaped queries,
libpqxx for anything complex. Introduce sqlpp23 if and when the query surface is
large enough that compile-time checking catches real bugs in practice.

**onnxruntime for in-process embedding**

The decision point (Section 14, point 3) recommends out-of-process for LLM and
in-process for embedding via ONNX Runtime. The justification is network latency
per embedding call. This is worth scrutinizing for an enterprise B2B on-prem SaaS:

- onnxruntime is a 40+ MB dependency with its own GPU memory management, CUDA
  version pinning, and a build that frequently breaks on non-mainstream toolchains
  (Gentoo in particular has had issues with its bundled XNNPACK and OpenMP).
- Every embedding model update requires a rebuild and redeployment of wikore-api
  and wikore-ingest. For enterprise on-prem customers, "deploy a new binary to
  update the embedding model" is a non-trivial change window.
- The loopback HTTP overhead for embedding is 0.5-2 ms per batch call. Embedding
  inference itself takes 20-200 ms per batch. The hop is not the bottleneck.
- llama-server already supports /v1/embeddings and /v1/rerank and is already
  validated in this environment (astraea.cpp uses it for both).

Verdict: **start with HTTP-based embedding via llama-server, same pattern as the
LLM**. This gives model-swap without recompile and removes onnxruntime from the
build entirely at MVP. Add in-process onnxruntime only if profiling on real
customer traffic shows loopback latency is a measured bottleneck, not a modeled one.

**opentelemetry-cpp**

The intent is right: bake tracing in from line one, not retrofit it. But
opentelemetry-cpp is one of the heaviest C++ dependencies in the ecosystem:
- Compile times measured in minutes even incremental
- API changes between minor versions (the 1.x -> 2.x transition broke most
  integration code)
- The C++ SDK is less mature than the Java/Go equivalents; several exporters
  are still unstable

For enterprise B2B SaaS, the customers who care about tracing either send to
Datadog/Grafana Cloud (via OTLP) or run Jaeger on-prem. That is a real
requirement at enterprise scale.

Verdict: **keep the intent, soften the implementation**. For MVP, structured JSON
logs with a `trace_id` field that correlates to a per-request UUID cover 80% of
the debugging value. Abstract the tracing port so you can plug in opentelemetry-cpp
later without touching application code. Add the real OTel SDK in the first release
that a paying customer asks for distributed traces, not before.

**boost::redis**

Good choice. First-class Asio/drogon coroutine support, actively maintained,
type-safe command interface. Keep it - but note: if you drop Asio in favour of
Drogon, boost::redis loses its main advantage (Asio integration). In a pure Drogon
stack you would use a hiredis async adapter or drogon's built-in Redis client
instead. Resolve after the Drogon-vs-Asio decision.

**glaze**

Correct choice for C++23. Reflection-based, header-only, substantially faster than
nlohmann for both parse and write, good error messages. No objections.

**CMake + Conan vs vcpkg**

Already in Objection 8. The astraea.cpp project uses vcpkg manifest mode. If both
projects live on the same machine, standardize on one. vcpkg manifest mode is
simpler for a single-language C++ project. Conan's profile system is valuable for
cross-compilation and multi-platform CI (Windows + Linux + macOS SDKs), which is
relevant if you plan to ship a Windows on-prem installer. If that is on the
roadmap, Conan is the better long-term choice. If Linux-only for the foreseeable
future, vcpkg is less friction.

---

### Section 14 - decision points

**Point 1: HTTP framework (Drogon vs Beast)**

The recommendation is correct (Drogon for MVP) but the cost-of-migration estimate
is too optimistic. "Re-write transport layer only" understates it: every handler
writes `const drogon::HttpRequestPtr& req, drogon::FilterCallback&&` signatures.
Every SSE stream uses drogon's `HttpResponsePtr::newAsyncStreamResponse`. If you
switch to Beast, you rewrite every handler. That is real work.

The flip side: if you pick Drogon now and fully commit (drop Asio from the text),
the transport layer stays thin and the migration cost stays bounded. The mistake
is hedging by listing Beast as the "correct answer" while implementing Drogon -
that hedge leaks into code ("should I use drogon::Task or asio::awaitable here?").

Decision: pick Drogon, close the question, update Sections 4 and 8 to say so.

**Point 2: SQL style (sqlpp23 vs libpqxx-raw)**

Argued above. Additional B2B angle: the first few enterprise customers will ask for
custom report exports, audit log queries, and bulk user management. Those queries
will be written under time pressure. A raw libpqxx query is debuggable in psql;
a sqlpp23 expression requires the DSL schema code to compile. For a small team,
defer sqlpp23 until the query surface is stable enough that compile-time checking
pays for itself.

**Point 3: In-process vs out-of-process LLM/embedder**

The in-process ONNX recommendation for embedding deserves a harder look (see
onnxruntime critique above). The real question for enterprise B2B is: who
controls the embedding model lifecycle? If the answer is "the wikore vendor ships
a tested model with each release," in-process is fine. If the answer is "the
customer can swap models without upgrading wikore," out-of-process is mandatory.
For an on-prem self-hosted product, the latter is the more honest answer.

**Point 4: WASM plugin host**

Agree, defer. The stub port in domain is enough.

**Point 5: Multi-region**

Agree, defer. The composite FK pattern already makes region-sharding straightforward
when needed (company_id prefix on everything).

**Point 6: OpenAPI vs gRPC for public API**

Correct. Enterprise customers integrating with internal tooling want a REST/JSON
API they can hit with curl. Add: generate the OpenAPI spec from code (or write the
spec first and generate stubs), not by hand. An out-of-date spec doc is worse than
no spec. Tools: oapi-codegen for spec-driven stub generation, or swagger-cpp if
you want C++ annotations. At minimum, commit the spec file and validate it in CI.

**Point 7: Single repo vs multi-repo**

Correct. The multiple-main() pattern (already in the proposed layout) gives you
independent deployment units without versioning pain. The only caveat: make sure
the four binaries (api, ingest, scheduler, eval) all live behind the same
CMakeLists target so `cmake --build` produces all four. Don't let them drift
into four separate CMake projects that share code by copy-paste.

---

### Missing decision points that should be in Section 14

**Point 8: gRPC for internal calls - decide now**

The library table says "grpc-cpp async" for worker<->api. If the decision is "use
Redis queues + Postgres for all inter-process communication at MVP," remove gRPC
from the dependency list entirely. If gRPC is kept, add a concrete decision:
"which calls go over gRPC, what is the .proto API surface, and when does it go live."
Leaving it listed but unscoped means it gets built halfway and dropped.

**Point 9: OTel SDK timing**

When does opentelemetry-cpp get added: day one (as currently implied) or post-MVP?
See critique above. This is a build-time cost decision, not an architectural one -
but it should be explicit.

**Point 10: Embedding model and Qdrant collection name**

Section 15 mentions this as an "open question" but it belongs in Section 14
because it blocks coding. The Qdrant collection name is baked into V008 and V003
comments. The vector dimension is baked into document_chunk_vectors schema
expectations. Choosing BGE-M3 (1024 dims) vs E5-large (1024) vs text-embedding-3-small
(1536 or 3072) determines the Qdrant collection setup and the chunk budget.
This should be decided before writing the ingest pipeline.

---

### Summary table (library + decision point items only)

| # | Severity | Item |
|---|----------|------|
| L1 | BLOCKER | Drogon + Asio: same blocker as above; pick one and update Section 4/8 |
| L2 | RECOMMENDED | Drop gRPC for internal calls at MVP; Redis queues suffice |
| L3 | RECOMMENDED | Start with HTTP-based embedding (llama-server); defer onnxruntime |
| L4 | RECOMMENDED | Defer opentelemetry-cpp to post-MVP; use trace_id in JSON logs until needed |
| L5 | OPTIONAL | Resolve boost::redis vs hiredis after Drogon-vs-Asio decision |
| L6 | OPTIONAL | Decide vcpkg vs Conan before first CMakeLists.txt (already in Objection 8) |
| D1 | REQUIRED | Section 14.1: commit to Drogon, stop hedging toward Beast in the document |
| D2 | REQUIRED | Section 14.2: defer sqlpp23; libpqxx / Drogon ORM everywhere at MVP |
| D3 | REQUIRED | Section 14.3: challenge in-process ONNX; start HTTP embedding, measure first |
| D4 | REQUIRED | Section 14 missing: decide gRPC vs Redis-queue for inter-process calls |
| D5 | REQUIRED | Section 14 missing: decide embedding model + Qdrant collection before ingest |
| D6 | OPTIONAL | Section 14.6 addition: commit to spec-driven OpenAPI (spec file in repo, CI-validated) |

— Reviewer: Claude Sonnet 4.6
