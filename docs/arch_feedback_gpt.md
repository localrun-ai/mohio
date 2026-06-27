# Wikore architecture feedback - concrete failure modes

Scope: critique of the proposed application architecture, especially the commitments to modular monolith, hexagonal layering, Boost.Asio coroutines, and retrieval-as-pipeline.

Verdict: the direction is good, but the current document is too optimistic in a few places. The main risks are not "C++ is hard" or "RAG is hard" in the abstract. The concrete risks are transaction boundaries, async ecosystem mismatch, duplicate side effects between Postgres/Qdrant/Redis, pipeline stages accidentally widening access, and operational coupling between API and workers.

I would not rewrite the architecture. I would add a few sharper contracts before line 1 of C++.

---

## 1. Executive summary

The proposal has the right high-level shape:

- Modular monolith plus extracted workers is a good fit for a serious v1.
- Hexagonal layering is useful if it protects domain rules from vendor-specific APIs.
- Asio coroutines are a good foundation for high-concurrency I/O.
- Retrieval-as-pipeline is the right mental model for RAG quality evolution.

But the proposal currently misses several failure modes:

1. The modular monolith still needs explicit distributed-side-effect boundaries. Postgres, Redis, and Qdrant will not update atomically.
2. The worker topology needs idempotency, leases, outbox semantics, and payload schema versioning, otherwise duplicate ingest and stale Qdrant payloads are inevitable.
3. Hexagonal layering can accidentally hide transaction boundaries behind ports. That is dangerous for audit, grants, document promotion, and wiki publication.
4. "One executor pool, all I/O co_await-able" is probably too clean. Drogon, gRPC, libpqxx, and CPU/GPU work may not all live naturally on one Asio executor.
5. The retrieval pipeline needs a hard security contract: every stage must prove it cannot widen access or leak chunks through logs, debug state, traces, or terminal outputs.
6. Cancellation and timeout behavior is under-specified. A cancelled chat request can leave Redis semaphores, Qdrant upserts, DB transactions, or LLM calls in partial states.
7. The current architecture underspecifies backpressure. Without it, LLM latency and ingest spikes will dominate the system, regardless of C++ performance.

Approval recommendation: proceed with this architecture only after adding the contracts listed in Section 12.

---

## 2. Modular monolith plus workers

### Commitment

The proposal uses one shared C++ codebase with multiple `main()` entry points:

- `wikore-api`
- `wikore-ingest`
- `wikore-scheduler`
- `wikore-eval`

This is the correct direction. The mistake would be starting with microservices.

### Failure mode 2.1: API and workers share code but not deployment timing

The document says extracting a module later is just moving an entry point. That is directionally true, but misses a real deployment problem: API, ingest, and scheduler can run different binary versions during rolling deploys.

Concrete failure:

1. API v2 writes a new ingest job shape to the outbox.
2. Ingest worker v1 is still running during deploy.
3. Worker v1 consumes the job and writes an old Qdrant payload shape.
4. API v2 retrieval expects the new fields and silently misses chunks.

Required contract:

- Every outbox job has `job_type`, `job_schema_version`, and `payload`.
- Every Qdrant payload has `payload_schema_version`.
- Workers must reject unknown future versions and leave the job retryable, not partially process it.
- Deploy order must be documented: schema first, workers that can read old and new next, API writers last.

### Failure mode 2.2: Postgres commit succeeds, Qdrant/Redis side effects fail

The architecture talks about workers and adapters, but does not explicitly define the consistency boundary.

Concrete failure:

1. User promotes document version v3 to active.
2. Postgres transaction commits.
3. Redis invalidation fails.
4. Qdrant lifecycle payload update fails.
5. Chat still retrieves v2 for some users for minutes or hours.

Required contract:

- Any state change that requires Redis/Qdrant follow-up must write an outbox row in the same Postgres transaction.
- A worker processes the outbox with retries and idempotency keys.
- The API never tries to make Postgres plus Qdrant plus Redis look atomic.
- User-facing reads must be tolerant of eventual Qdrant lag, either by fallback hydration from Postgres or by surfacing "indexing in progress".

### Failure mode 2.3: ingest duplicate work creates duplicate chunks or orphan vectors

The proposal says ingest pulls jobs from outbox, but does not define job claiming, idempotency, or partial retry semantics.

Concrete failure:

1. Worker claims document version v4.
2. It inserts chunks into Postgres.
3. It upserts half the vectors to Qdrant.
4. Worker crashes before marking the job done.
5. Retry runs again and inserts duplicate vectors or fails on uniqueness.

Required contract:

- Ingest job idempotency key should be `(company_id, document_version_id, embedding_model_id)`.
- Postgres chunk insert should be idempotent by `(company_id, document_version_id, chunk_index)`.
- Qdrant point IDs should be deterministic from `(chunk_id, embedding_model_id)` or stored before upsert.
- Retrying ingest must be safe after crash at every step.
- A stuck `processing` document version must be detectable and recoverable.

### Failure mode 2.4: scheduler as single replica becomes both SPOF and split-brain risk

The proposal says scheduler is a single replica. That avoids duplicate cron work, but creates a single point of failure. Running two schedulers later creates split-brain unless the jobs are guarded.

Concrete failure:

- Two scheduler instances accidentally run after a deployment change.
- Both sweep tombstones or run SCIM resync.
- Both enqueue duplicate Qdrant deletion jobs.

Required contract:

- Scheduler tasks must use Postgres advisory locks or a DB-backed lease table.
- Every scheduled task must be idempotent.
- The system should be safe if two schedulers run accidentally.

### Failure mode 2.5: no noisy-neighbor boundary for ingest

Ingest is GPU-bound and bursty. A single tenant uploading a large corpus can starve other tenants.

Required contract:

- Per-tenant ingest queues or weighted fair scheduling.
- Per-tenant concurrency limits.
- Per-tenant Qdrant upsert rate limits.
- Admin-visible ingest backlog and stuck job status.

---

## 3. Hexagonal layering

### Commitment

Transport -> Application -> Domain -> Ports -> Adapters.

This is good, but only if ports do not erase transaction and consistency requirements.

### Failure mode 3.1: ports hide transaction boundaries

The document says audit writes happen in the same transaction as privileged state changes, using an `AuditSink` port. That can fail if `AuditSink` is just another adapter call.

Concrete failure:

```cpp
co_await grantAccessRepo.grant(...);
co_await auditSink.record(...);
```

If the second call fails, the grant already committed. If the first call commits inside the repo, the use case cannot guarantee atomicity.

Required contract:

- Introduce an explicit `UnitOfWork` or `Transaction` port.
- Use cases that mutate state receive or create a transaction.
- Repositories and audit sink operate inside that transaction.
- The transaction owns commit/rollback.

Shape:

```cpp
struct UnitOfWork {
    DocumentRepo& documents();
    AccessRepo& access();
    AuditSink& audit();
    OutboxRepo& outbox();
    asio::awaitable<Result<void>> commit();
    asio::awaitable<void> rollback();
};
```

### Failure mode 3.2: too many general-purpose ports become god interfaces

`DocumentRepo`, `AccessResolver`, `Retriever`, `Reranker`, `ChunkStore`, etc. are reasonable names, but general ports tend to grow until every use case depends on everything.

Concrete failure:

- `DocumentRepo` ends up with 70 methods.
- Tests require huge mocks.
- Every feature touches the same adapter.
- Interface stability becomes worse than direct SQL.

Required contract:

- Prefer narrow ports grouped by use case or aggregate:
  - `DocumentVersionWriter`
  - `DocumentVersionReader`
  - `WikiPublisherRepo`
  - `AccessScopeReader`
  - `AuditWriter`
- Avoid one generic `DocumentRepo` unless it stays small.

### Failure mode 3.3: domain purity conflicts with database-owned invariants

The proposal says domain is pure and the database is the source of truth for CHECKs, FKs, and triggers. That is good, but the application still needs a clear error taxonomy when DB rejects writes.

Concrete failure:

- Postgres rejects a partial unique index on active document version.
- Adapter maps it to `Error::DatabaseError`.
- Transport returns HTTP 500 instead of 409 Conflict.

Required contract:

- Adapter maps SQLSTATE and named constraints to domain/application errors.
- Constraint names are part of the app contract.
- Common errors are typed:
  - `Conflict`
  - `NotFound`
  - `PermissionDenied`
  - `InvalidState`
  - `StaleVersion`
  - `RateLimited`

### Failure mode 3.4: "swappable adapters" is oversold

The doc says Qdrant -> Weaviate or Postgres -> YugabyteDB is one new adapter. This is too optimistic.

Concrete issue:

- Qdrant payload filters, MatchAny semantics, vector collection design, payload indexes, and point IDs affect the domain contract.
- Postgres triggers, partial indexes, composite FKs, and SQL functions are part of the core model.

Better wording:

- Adapters isolate client APIs.
- They do not make semantic databases interchangeable.
- Swapping Qdrant is plausible only if a `RetrieverCapabilities` contract is defined.
- Swapping Postgres is not a v1 goal.

### Failure mode 3.5: RequestContext can become a dumping ground

`RequestContext` contains tenant, principal, scope, trace span, deadline, and clock. That is useful, but it can grow into a global object by another name.

Concrete failure:

- Feature flags, DB handles, config, logger, locale, permissions, and caches get added.
- Every method takes a giant object.
- Tests become harder because every context must be fully constructed.

Required contract:

- `RequestContext` contains only request identity, deadline, trace, and clock.
- No service locators.
- No adapters.
- No mutable business state.

---

## 4. Boost.Asio coroutine model

### Commitment

C++23 coroutines on Boost.Asio, one executor pool, threads = cores, never block on I/O.

This is attractive, but the ecosystem will not be that clean in practice.

### Failure mode 4.1: Drogon and "one Asio executor" may not align

The architecture says Drogon for MVP and Asio as the coroutine runtime. That needs an explicit integration decision. If the HTTP framework has its own event loop model, `RequestContext` cancellation, tracing, and coroutine scheduling may not be uniform.

Concrete failure:

- HTTP handler runs on framework event loop.
- Application coroutine runs on Asio executor.
- gRPC callback runs on gRPC completion threads.
- Logging/tracing context is lost during hops.

Required contract:

- Document the actual executor boundary for each transport.
- If Drogon is used, define whether application use cases are called directly, posted into Asio, or written in Drogon coroutine style.
- Add a small spike before full coding: one HTTP endpoint -> Postgres -> Redis -> cancellation -> trace.

### Failure mode 4.2: gRPC async is not automatically Asio-native

The doc says grpc-cpp async plus Asio coroutines. That can work, but it is not free. gRPC has its own async APIs and completion/callback model.

Concrete failure:

- A Qdrant gRPC call cannot be simply `co_await`-ed without a wrapper.
- Cancellation deadline is not propagated to gRPC `ClientContext`.
- Completion callbacks run on a different thread and resume coroutine unsafely.

Required contract:

- Pick one integration approach:
  - callback API wrapped into Asio awaitables,
  - CompletionQueue bridge,
  - a known asio-grpc style adapter,
  - or isolate gRPC behind a dedicated thread pool and await futures.
- Standardize deadline propagation into every gRPC call.
- Standardize cancellation propagation.

### Failure mode 4.3: libpqxx async may not fit the ideal coroutine model

The doc says `libpqxx async` for hot retrieval path. Verify this early. If the database adapter is not truly coroutine-native, the whole "never block on I/O" claim becomes fragile.

Concrete failure:

- A supposedly async Postgres query blocks an Asio worker thread.
- Under LLM latency, the system still works.
- Under high DB concurrency, request latency spikes and ready checks fail.

Required contract:

- Build a DB adapter spike before use cases.
- Run 500 concurrent simple queries and confirm no executor starvation.
- If truly async integration is awkward, use a dedicated DB worker pool and call it honestly `blocking_db_pool`.

### Failure mode 4.4: CPU-bound work on the I/O executor

The doc says one executor pool, threads = cores, never block on I/O. But many operations are CPU-bound:

- JSON parse/serialize
- JWT validation
- PDF parsing
- chunking
- token counting
- prompt construction
- cross-encoder reranking if local
- ONNX embedding if in-process

Concrete failure:

- Large JSON or document parsing runs on the same executor as HTTP sockets.
- Readiness checks time out during ingest spikes.
- Tail latency explodes.

Required contract:

- Separate executor pools:
  - I/O executor
  - CPU executor
  - blocking executor
  - GPU/concurrency semaphore for embedding/reranking
- Make pool crossing explicit with named helpers.
- Add metrics for queue depth per executor.

### Failure mode 4.5: cancellation safety is harder than the doc implies

The doc says cancellation propagates with `asio::cancellation_signal`. That is not enough.

Concrete failure:

1. Request is cancelled after Redis semaphore acquired.
2. Coroutine unwinds before releasing semaphore.
3. LLM capacity leaks until TTL or manual reset.

Other examples:

- DB transaction left open until connection timeout.
- Qdrant upsert already sent, but Postgres job not marked done.
- Audit write skipped because cancellation happened after state mutation.

Required contract:

- Use RAII guards for semaphores, transactions, and leases.
- Define cancellation points.
- After a state mutation begins, either shield commit/audit/outbox from cancellation or compensate explicitly.
- Separate user cancellation from server shutdown cancellation.

### Failure mode 4.6: coroutine frame lifetime and references

The proposal says `RequestContext&` is passed everywhere and never stored. That is good, but coroutines make accidental reference capture dangerous.

Concrete failure:

```cpp
auto task = [&, query]() -> asio::awaitable<void> {
    co_await stage.run(ctx, query, prev);
};
```

The coroutine outlives the stack frame that owned `ctx` or `prev`.

Required contract:

- Ban detached coroutines that capture references.
- Use structured concurrency: every spawned task is awaited or owned by a cancellation scope.
- Use `shared_ptr<const RequestContext>` only if lifetime cannot be lexically guaranteed.
- Add clang-tidy/checklist rules around coroutine captures.

---

## 5. Retrieval-as-pipeline

### Commitment

Pipeline stages:

`CuratedAnswerStage -> AccessFilterStage -> SensitivityFilterStage -> VectorRetrievalStage -> SectionExpansionStage -> BM25Stage -> RerankerStage -> ContextBuilderStage`

This is the right shape, but the current stage interface is too permissive.

### Failure mode 5.1: stages can widen access after AccessFilterStage

The document says stages must not widen access. That is a comment, not a guarantee.

Concrete failure:

- `SectionExpansionStage` retrieves parent and sibling chunks by `section_id` from Postgres.
- It forgets to re-check `access_scope_ids` or lifecycle/sensitivity.
- A restricted sibling chunk enters the context.

Required contract:

- `RetrievalResult` should carry an `AccessEnvelope` or `EvidencePolicy` created by `AccessFilterStage`.
- Any stage that adds chunks must call a shared `EvidenceGate`.
- Do not let stages query raw chunks directly.
- Make unauthorized chunk insertion impossible in tests.

Shape:

```cpp
struct EvidenceGate {
    bool allows(const ChunkCandidate&) const;
    asio::awaitable<std::vector<ChunkCandidate>> hydrate_allowed(...);
};
```

### Failure mode 5.2: terminal curated answers can bypass audit and source policy

`CuratedAnswerStage` can return `terminal = true`. That is useful, but dangerous.

Concrete failure:

- Curated answer contains attached document versions.
- The stage returns terminal before normal citation/audit logic runs.
- Chat turn is stored without version-pinned sources.

Required contract:

- Terminal result still goes through `AnswerFinalizerStage`.
- Audit, citations, sensitivity, and chat persistence are outside the retrieval pipeline or enforced after it.
- `terminal` means "skip retrieval", not "skip compliance".

### Failure mode 5.3: pipeline config can create invalid stage orders

The doc says tenant-specific policy can use different stage ordering. That is powerful but can break security.

Concrete failure:

- Tenant config puts `SectionExpansionStage` before `AccessFilterStage`.
- Tenant config removes `SensitivityFilterStage`.
- A/B test pipeline accidentally omits citation building.

Required contract:

- Pipeline builder validates stage graph.
- Security stages are mandatory and fixed-order.
- Optional stages can only live in approved slots.

Recommended model:

```text
Mandatory precondition:
  BuildAccessPolicy

Candidate-producing stages:
  VectorRetrieval, BM25, CuratedAnswer

Mandatory gate after every candidate-producing or candidate-expanding stage:
  EvidenceGate

Optional enrichment:
  SectionExpansion, Reranker

Mandatory finalization:
  ContextBuilder, CitationBuilder, AuditEnvelope
```

### Failure mode 5.4: RetrievalResult becomes a giant mutable bag

A pipeline often starts clean and becomes:

```cpp
struct RetrievalResult {
    vector<Chunk> chunks;
    vector<Doc> docs;
    json debug;
    bool terminal;
    string answer;
    vector<string> warnings;
    map<string, any> stage_state;
};
```

That creates hidden coupling between stages.

Required contract:

- Keep `RetrievalResult` small and typed.
- Store debug info in a separate `RetrievalTrace` object.
- Avoid `std::any` or unstructured JSON as stage communication.
- Distinguish candidates, selected context, and final answer.

### Failure mode 5.5: reranker can reorder into policy violation

Rerankers should not be able to resurrect filtered candidates.

Concrete failure:

- Vector stage retrieves allowed chunks.
- Section expansion adds some unverified siblings.
- Reranker scores all chunks.
- Context builder takes top N, including a sibling that should have been dropped.

Required contract:

- Reranker input type is `AllowedCandidate`, not `ChunkCandidate`.
- Only `EvidenceGate` can construct `AllowedCandidate`.

### Failure mode 5.6: logging/tracing can leak restricted content

The architecture says OTel spans on LLM call with prompt and completion length. Good. But traces and logs must never include raw prompt text by default.

Concrete failure:

- A debug span attribute stores prompt preview.
- Logs are sent to a central system with broader admin access than Wikore itself.
- Restricted HR content leaks through observability.

Required contract:

- Logs/traces may include IDs, counts, hashes, token lengths, model name, latency.
- Raw chunk text, prompt text, completion text, and user questions are off by default.
- Debug capture requires tenant-level opt-in and redaction.

---

## 6. Data flow and consistency risks

### Failure mode 6.1: Qdrant is treated as source of truth in retrieval

The schema correctly stores chunk text in Postgres. The architecture should state that Qdrant is an index, not the evidence store.

Required contract:

- Qdrant returns point IDs and scores.
- Postgres hydrates exact chunk/version/section/sensitivity data.
- Context builder uses Postgres-hydrated text, not Qdrant payload text.
- If Qdrant payload disagrees with Postgres, Postgres wins and a resync job is queued.

### Failure mode 6.2: access-scope cache staleness during grant revocation

Reverse-index Redis sets reduce invalidation cost, but revocation still needs a strict sequence.

Concrete failure:

1. Admin revokes Legal access to HR document.
2. Postgres commits.
3. Redis invalidation fails.
4. Old `lr:eff` cache still allows retrieval until TTL.

Required contract:

- Privileged grant/revoke writes an outbox event in same transaction.
- API can perform best-effort immediate invalidation, but worker retry is authoritative.
- For high-risk revokes, either shorten TTL or mark a company-level `access_epoch` so old caches are rejected.

Consider:

```text
lr:access_epoch:{company_id} = integer
lr:eff value includes epoch
if current epoch != cached epoch, discard cache
```

This avoids relying only on deleting keys.

### Failure mode 6.3: historical retrieval semantics can diverge between Postgres and Qdrant

If historical search uses Qdrant payload fields `activated_at` and `superseded_at`, Qdrant must be updated whenever versions are promoted/deprecated. If Qdrant lags, historical retrieval may be wrong.

Required contract:

- Version promotion writes Qdrant payload update job in outbox.
- Historical retrieval can optionally post-filter by Postgres after Qdrant returns candidates.
- Historical retrieval should not trust Qdrant alone for final eligibility.

---

## 7. Library choice risks

### Failure mode 7.1: Drogon now, Beast later may not be cheap

The document says transport is thin, so migration from Drogon to Beast is transport-only. That is true only if no Drogon types leak out.

Required contract:

- No Drogon request/response types outside `src/transport/http`.
- No Drogon JSON type outside transport.
- Application input/output DTOs are framework-independent.
- Integration test should call use cases directly, not only HTTP.

### Failure mode 7.2: two SQL styles can split the team and duplicate mapping logic

The proposal uses sqlpp23 for app code and raw libpqxx for hot retrieval. This is reasonable, but it creates two query/mapping idioms.

Concrete failure:

- Same domain object has two mappers.
- One mapper populates `sensitivity_label`; the other forgets.
- A security bug only appears on the hot path.

Required contract:

- One row-to-domain mapper per table/aggregate.
- Raw SQL path must use the same mapping functions.
- Benchmark before choosing raw pqxx for a path.

### Failure mode 7.3: in-process ONNX embedder can break API isolation

The doc recommends in-process local embedder. That may be fine for ingest, but risky in API.

Concrete failure:

- Query embedding runs in-process on API.
- ONNX runtime uses large thread pools or GPU resources.
- API latency becomes noisy under concurrent chat.

Required contract:

- In-process embedder is allowed in ingest worker first.
- API query embedding must have a concurrency semaphore and separate executor.
- If noisy, move embedder out of process behind the same `Embedder` port.

### Failure mode 7.4: glaze everywhere may not fit all JSON needs

Glaze may be very fast, but using it everywhere can make dynamic JSON payloads awkward.

Required contract:

- Use typed structs for API DTOs, Qdrant payloads, and LLM messages.
- Use JSONB payloads only at boundaries where shape is intentionally flexible.
- Do not pass arbitrary JSON through domain logic.

---

## 8. Security and authorization gaps

### Failure mode 8.1: OIDC tenant mapping is under-specified

The document says reject requests where token tenant claim disagrees with URL path. That assumes every token has a reliable tenant claim.

Concrete failure:

- Customer OIDC token has issuer and subject but no tenant claim.
- User belongs to multiple companies.
- URL path decides tenant, but token mapping is ambiguous.

Required contract:

- Define `TenantResolver` explicitly:
  - issuer + subject -> user records
  - user record -> company_id
  - URL company slug -> company_id
  - require exact match
- For multi-company users, require explicit company selection and membership.

### Failure mode 8.2: service-to-service auth is hand-waved

Workers need to call API or write DB directly. The architecture should decide.

Options:

1. Workers write DB directly using service DB role.
2. Workers call internal gRPC APIs.
3. Mixed model.

Failure if undecided:

- Ingest worker bypasses application use case and forgets audit/outbox.
- Scheduler performs direct DB changes that API invariants do not expect.

Required contract:

- For v1, workers can write DB directly only through the same repository/use-case library.
- Service identity must be represented in audit log.
- Internal gRPC auth can come later, but DB roles should not be superuser-like.

### Failure mode 8.3: audit in same transaction conflicts with async outbox side effects

Audit rows should record the privileged intent and committed DB state. Qdrant/Redis side effects are eventually consistent.

Required contract:

- Audit row for state change is in transaction.
- Outbox row for side effects is in transaction.
- Separate audit or job log records whether side effects completed.
- Admin UI can show "grant committed, index resync pending".

---

## 9. Observability and operations risks

### Failure mode 9.1: high-cardinality metrics

The doc says never label by user_id, only tenant_id or feature. Tenant ID can also become high-cardinality if Wikore grows or if test tenants are many.

Required contract:

- Tenant labels only on a limited set of business metrics.
- Low-level latency metrics should use route, operation, dependency, status.
- Per-tenant cost accounting goes to DB events, not Prometheus labels.

### Failure mode 9.2: readiness checks can amplify outages

`/readyz` checking all downstreams can cause cascading restarts if Qdrant or Redis has a transient issue.

Required contract:

- `/healthz`: process alive.
- `/readyz`: only dependencies required to accept basic traffic.
- Degraded dependencies should report degraded, not necessarily fail readiness.
- Use separate deep health endpoint for operators.

### Failure mode 9.3: graceful shutdown needs lease and semaphore cleanup

The doc says SIGTERM drains in-flight coroutines. Good, but workers also hold leases and semaphores.

Required contract:

- On shutdown, stop claiming new jobs.
- Finish or release current job lease.
- Release Redis semaphores via RAII or TTL.
- Mark interrupted jobs retryable.

---

## 10. Test strategy gaps

### Failure mode 10.1: integration tests without fault injection will miss real bugs

Docker-compose tests are necessary, but not enough.

Add tests for:

- Postgres commit succeeds, Qdrant fails.
- Redis invalidation fails.
- Worker crashes after chunks inserted, before vectors upserted.
- Worker crashes after vectors upserted, before job done.
- LLM request times out after semaphore acquired.
- User access revoked while chat request is in progress.
- Qdrant returns chunk whose Postgres version is no longer eligible.

### Failure mode 10.2: property tests should target security invariants first

Property tests are mentioned for access-scope resolver and pipeline composition. Good. Make them concrete:

- A user never gets chunks outside their resolved scope.
- Adding a membership can only increase or preserve visible scopes.
- Removing a membership can only decrease or preserve visible scopes after cache invalidation.
- Moving an org subtree preserves closure consistency.
- Section expansion never introduces a chunk not allowed by EvidenceGate.

### Failure mode 10.3: no contract tests for Qdrant payload schema

Required tests:

- Upserted payload contains required fields.
- Payload schema version is current.
- Historical retrieval payload fields exist.
- Sensitivity label exists.
- Section metadata exists if section-aware retrieval is enabled.
- Postgres hydration rejects stale/wrong payloads.

---

## 11. Open design decisions to close before coding

These are not all blockers, but each should be explicitly decided.

### Decision 11.1: what is the transaction model?

Options:

- Use case owns `UnitOfWork`.
- Repository methods self-commit.

Recommendation: use case owns `UnitOfWork` for any mutation. Read-only queries can use direct repository calls.

### Decision 11.2: what is the outbox model?

Required for:

- Qdrant upserts/deletes
- Redis invalidation retries
- ingest jobs
- document promotion side effects
- tombstone deletion jobs

Recommendation: DB outbox with idempotency keys and typed payload version.

### Decision 11.3: what is the executor model?

The proposal says one executor pool. I recommend:

- I/O executor
- CPU executor
- blocking executor
- GPU semaphores

At minimum, document which operations may run on which pool.

### Decision 11.4: how are workers authorized?

Recommendation: service identity in audit, least-privilege DB roles, no bypass of use-case library.

### Decision 11.5: how does retrieval enforce policy between stages?

Recommendation: mandatory `EvidenceGate`; only `AllowedCandidate` can enter reranker/context builder.

---

## 12. Concrete changes I would make to architecture.md

### Must add before C++ starts

1. Add `UnitOfWork` / transaction boundary section.
2. Add outbox/idempotency section for Postgres -> Redis/Qdrant side effects.
3. Replace "one executor pool" with an explicit executor model:
   - I/O
   - CPU
   - blocking
   - GPU semaphores
4. Add gRPC/Asio integration decision or spike requirement.
5. Add retrieval `EvidenceGate` contract.
6. Add pipeline stage ordering validation contract.
7. Add cancellation safety rules for transactions, leases, and semaphores.
8. Add payload schema versioning for outbox and Qdrant payloads.
9. Add "Qdrant is index, Postgres is evidence source" rule.
10. Add observability redaction rule: no raw prompts/chunks in logs/traces by default.

### Should add soon

1. Per-tenant ingest fairness and quotas.
2. Scheduler advisory lock / lease contract.
3. Worker crash recovery matrix.
4. Constraint-name to domain-error mapping.
5. Transport leakage rule for Drogon/Beast migration.
6. Test fault-injection matrix.

---

## 13. Revised approval status

I approve the high-level commitments:

- Modular monolith plus extracted workers: yes.
- Hexagonal layering: yes, if transaction boundaries are explicit.
- Boost.Asio coroutines: yes, if executor boundaries are honest.
- Retrieval pipeline: yes, if EvidenceGate is mandatory and security stages cannot be reordered away.

I do not approve the current architecture document as a coding contract yet.

The missing pieces are small but important. Without them, the first C++ implementation will likely bake in one or more of these bugs:

- repository methods that self-commit and make audit/outbox non-atomic;
- pipeline stages that can add unauthorized chunks;
- gRPC or DB calls that block the Asio executor;
- cancelled requests that leak semaphores or leave jobs half-done;
- Qdrant payload drift that retrieval trusts too much;
- workers that duplicate or corrupt ingest after crashes.

Add the contracts in Section 12, then start coding.

---

## 14. First implementation slice I would choose

Do not start with full HTTP chat.

Start with this vertical but non-user-facing slice:

1. Migration runner and smoke tests.
2. Postgres connection pool and `UnitOfWork`.
3. Outbox table access and idempotent job claim.
4. Company/root-org repository read path.
5. Document version promotion through SQL function.
6. Audit write in the same transaction.
7. Outbox event emitted in the same transaction.
8. Worker reads outbox event and marks it done idempotently.

Then implement retrieval:

1. Access resolver.
2. Qdrant filter builder.
3. Vector retrieval adapter.
4. Postgres hydration.
5. EvidenceGate.
6. Context builder.

Only after that add the HTTP chat endpoint.

That order proves the architecture before the UI/API surface starts depending on it.
