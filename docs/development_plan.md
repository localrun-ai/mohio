# Wikore Development Plan

Iteration-based plan to build wikore from the validated schema (V001–V014)
to a working enterprise RAG + wiki product. Each iteration has explicit
scope, exit criteria, and a **test session** that validates corpus
consistency and/or retrieval correctness before the next iteration starts.

Companion docs:
- `architecture.md` (with deltas from `arch_feedback_opus.md`)
- `review_feedback_v2.md` (schema state)

---

## Guiding principles

1. **Build the platform before the product.** Iteration 0 ships zero
   user-visible features and is mandatory.
2. **Every iteration ends with a test session.** A green test session is
   the gate; no "we'll catch up on tests next sprint."
3. **The test corpus is real, not synthetic placeholder.** A shared
   versioned corpus (see §10) is the contract against which every
   iteration is measured.
4. **Retrieval correctness is the spine.** Speed, polish, and features
   come after retrieval is provably correct.
5. **No "let's just check it works manually" milestones.** Each
   iteration produces an automated test that runs in CI from then on.

---

## Iteration 0 — Foundation (no user-visible value)

**Duration estimate:** 1.5–2 weeks

**Scope:** the platform on which iterations 1–5 sit. Nothing here is
optional; every later iteration depends on it.

- Repository scaffold: CMake + vcpkg manifest, `src/` layout per
  architecture.md §9, four `main()` entry points wired (api, ingest,
  scheduler, eval), all compiling and printing version info.
- Configuration: `config.toml` schema, typed loader via toml++,
  per-tenant overrides read from `companies.settings` JSONB.
- Logging: spdlog with JSON sink, `trace_id` per request (UUID),
  no-op `Tracer` port.
- Postgres adapter: Drogon's async PG client wrapped in a connection
  pool with configurable size. **UnitOfWork** primitive with explicit
  `begin()`, `commit()`, `rollback()`.
- **Outbox migration** (V015): new `outbox_events` table per
  `arch_feedback_opus.md` §3.2.
- `pg_error_mapper` adapter with exhaustive constraint-name mapping
  per `arch_feedback_opus.md` §3.8.
- Cross-cutting types: `Tenant`, `Principal`, `RequestContext`,
  `Result<T>` (using C++23 `std::expected`).
- One end-to-end use case implemented as an exemplar:
  `PromoteDocumentVersion` (calls SQL function from V010, writes audit
  log + outbox event in the same UoW).
- Outbox claim worker stub in `wikore-scheduler` (claim + log + ack;
  no real downstream side effect yet).

### Exit criteria

- All four binaries compile and start.
- Schema smoke tests (V001–V015) pass.
- The `PromoteDocumentVersion` use case is exercised via a C++ unit test:
  insert fixtures, call the use case, assert (a) version is promoted,
  (b) audit_log row created in the same transaction, (c) outbox_event
  row created in the same transaction, (d) scheduler picks up the
  outbox event and marks it complete.
- Postgres concurrency spike: 500 concurrent simple queries through the
  Drogon PG client with no executor starvation (per GPT 4.3 verification
  requirement).

### Test session #0: platform validation

Not a corpus test yet — the platform is tested before data flows
through it.

| Test | What it proves |
|---|---|
| schema smoke test (existing) | DB migrations correct end-to-end |
| `PromoteDocumentVersion` integration test | UnitOfWork + audit + outbox atomicity |
| Outbox claim race test | Two scheduler instances claiming the same job → exactly one wins (FOR UPDATE SKIP LOCKED) |
| Cancellation safety test | Cancel a coroutine mid-UoW → transaction rolls back, no orphan rows |
| PG concurrency spike | 500 concurrent reads, p99 < target latency, no thread pool starvation |
| Constraint mapping test | Trigger every named constraint, assert mapped to the right typed error |

---

## Iteration 1 — Ingest path and corpus consistency

**Duration estimate:** 2–3 weeks

**Scope:** documents flow from upload to searchable state.

- `DocumentVersionWriter` adapter.
- Ingest worker (`wikore-ingest`) consumes
  `lr:ingest:q:{company_id}` Redis lists. Per-tenant fair scheduling
  (round-robin across tenant lists).
- Document parsers. Section hierarchy (K5) extracted during parse and
  written to `document_sections`.
  - **PDF**: poppler (vcpkg) for text + heading detection via font-size
    heuristics or tagged PDF structure.
  - **DOCX**: own implementation (~300 lines) using pugixml + miniz
    (both vcpkg, header-friendly). Unzip the `.docx`, parse
    `word/document.xml`, walk `<w:p>` elements: heading level from
    `<w:pPr><w:pStyle w:val="HeadingN"/>`, text from `<w:t>` runs,
    tables flattened cell-by-cell. No third-party DOCX library
    (duckX/minidocx do not expose paragraph style names reliably,
    which is required for section hierarchy).
  - **HTML**: lexbor (vcpkg) walking heading tags h1-h6.
  - **Plain text / Markdown**: line-based heading detection (# / ## or
    ALL CAPS line heuristic); no external parser needed.
- Chunker: target 600-char chunks with 100-char overlap, respects
  section boundaries (chunk does not span sections). `section_id` set
  on `document_chunks`.
- Embedding adapter: HTTP client to llama-server `/v1/embeddings`,
  model = bge-m3, dim = 1024.
- Qdrant adapter: HTTP REST client, collection `wikore_chunks_v1`,
  upsert with deterministic point ID = `uuid_v5(chunk_id ||
  embedding_model_id)`. Payload includes `company_id`,
  `access_scope_ids`, `sensitivity_label`, `document_version_id`,
  `section_id`, `activated_at`, `superseded_at`, plus the
  `payload_schema_version` (starts at 1).
- Idempotent chunk insert by `(company_id, document_version_id,
  chunk_index)` per V003 unique index.
- Document version promotion through `promote_document_version()` SQL
  function (V010).
- Outbox event types implemented:
  `qdrant_upsert_chunk_payload`, `qdrant_delete_chunk_point`,
  `redis_invalidate_eff_keys`.
- Resync worker in `wikore-scheduler` consumes
  `outbox_events WHERE job_type IN ('qdrant_...')`.
- Polling fallback: scheduler re-enqueues `document_versions WHERE
  ingest_status='pending' AND created_at < now() - interval '15 min'`.

### Exit criteria

- A document can be uploaded, ingested, and queried via direct Qdrant
  call (no retrieval pipeline yet) and return the expected chunk IDs.
- Re-running ingest on the same document produces zero duplicate
  chunks, zero duplicate Qdrant points.
- Killing the ingest worker mid-batch and restarting resumes without
  corruption.
- Postgres `document_chunks` row count for any version equals Qdrant
  point count for that version (consistency invariant).

### Test session #1: corpus consistency

Uses the shared test corpus (see §10). Loaded fresh into clean
Postgres + Qdrant + Redis. All 50 documents ingest successfully, all
sections extracted, all chunks embedded.

| Test | What it proves |
|---|---|
| Cold ingest the full 50-doc corpus | End-to-end ingest pipeline works on real document shapes (PDF, DOCX, HTML, plain text) |
| Re-ingest the same corpus | Idempotency: row counts unchanged, Qdrant point counts unchanged |
| Kill ingest mid-batch, restart | Crash recovery: no orphan chunks, no orphan vectors, document_version eventually reaches `done` |
| Postgres ↔ Qdrant invariant | For every `document_versions` row in `lifecycle_status='active'`, `count(document_chunks) == count(Qdrant points for that version)` |
| Payload schema contract test | Every upserted Qdrant point has every required field (sensitivity, scope, version, section, lifecycle ts) with correct types and the current schema_version |
| Section hierarchy roundtrip | For each document, the section tree matches the source's heading structure exactly |
| Tombstone test | Delete a document → all chunks marked tombstoned in `document_chunk_tombstones`, all Qdrant points deleted within N seconds |
| Per-tenant fairness test | Two tenants upload 100 docs each simultaneously; neither is starved beyond a 2× ratio |

---

## Iteration 2 — Access resolution and retrieval primitives

**Duration estimate:** 2 weeks

**Scope:** the security boundary and the read path. The most important
iteration in the project.

- `AccessResolver` port + Postgres adapter. Resolves a `Principal` ×
  `Tenant` to an `AccessScope` (org_unit_ids transitively reachable via
  memberships, time-expiry-aware).
- Redis cache adapter for `lr:eff:*` with reverse-index Sets per V008.
  `EXPIRE` refreshed on every `SADD` per V2-N1 fix.
- `access_epoch` integer per company in Redis for emergency revocation
  (per `arch_feedback_opus.md` §3.7); `lr:eff` values include the
  epoch they were computed under.
- `QdrantFilterBuilder` (pre-processing step): converts `AccessScope`
  + `Principal.is_admin` + `sensitivity_label` policy into a Qdrant
  filter spec (MatchAny on `access_scope_ids`, MatchValue on
  `lifecycle_status='active'` for current retrieval).
- `VectorRetriever` adapter: Qdrant HTTP client, takes filter spec +
  query embedding, returns point IDs + scores.
- Postgres hydration: from point IDs back to typed
  `ChunkCandidate` with chunk text, document version, section,
  sensitivity, lifecycle timestamps. **Qdrant is index, Postgres is
  evidence** (per §3.6 of arch response).
- **EvidenceGate** type contract per §3.4 of arch response. Only
  `AllowedCandidate` can travel past it.
- Section expansion stage (K5): fetches parent + sibling chunks by
  `section_id`, runs them through EvidenceGate.
- Historical retrieval support: `as_of` timestamp parameter clamps
  to `activated_at <= as_of AND (superseded_at IS NULL OR
  superseded_at > as_of)`.

### Exit criteria

- Access resolver returns the correct org_unit set for a known fixture
  user × tenant pair.
- Cache hit rate >95% on warm traffic; <1ms p99 on cache hit.
- A property test (1000 trials, rapidcheck) confirms: for random
  principals, no chunk outside their resolved scope ever appears in the
  retrieval result.
- A type-system check (compilation test) confirms: `Reranker::rerank()`
  cannot accept a `ChunkCandidate` (only `AllowedCandidate`).
- Historical retrieval as-of any past date returns version-pinned
  results matching expected fixtures.

### Test session #2: retrieval correctness

The most important test session in the project. The corpus contains
deliberately-crafted access traps: HR-only documents accessible to
non-HR users would be the breach scenario.

| Test | What it proves |
|---|---|
| Golden questions × expected chunks (positive set) | Retrieval finds the right chunks for known queries |
| Cross-scope leakage (property test, 10000 trials) | No user × random-query pair ever produces a chunk outside their resolved scope |
| Sensitivity leakage (property test) | Guest users never see chunks with sensitivity ≥ confidential |
| Tombstone respect | Deleted documents return zero results within N seconds of deletion |
| Historical as-of-date | Retrieval at past timestamps returns the version that was active then, not the current version |
| Section expansion | When section expansion is enabled, sibling chunks are added IFF they pass EvidenceGate |
| Reranker resurrection attempt (compilation test) | Code that tries to add `ChunkCandidate` to a reranker's input fails to compile |
| Access cache invalidation | Revoke a membership; new requests within 100ms see the revocation (via access_epoch path); confirm Sets-based path catches it within 5min anyway |
| Move-org-unit during retrieval | Concurrent `move_org_unit()` does not corrupt in-flight retrieval results |
| Tenant boundary (property test, 10000 trials) | No request with tenant A ever sees a chunk owned by tenant B |

---

## Iteration 3 — LLM pipeline and the chat endpoint

**Duration estimate:** 2 weeks

**Scope:** the first user-visible product surface.

- `LlmProvider` port + llama-server HTTP adapter (OpenAI-compatible
  `/v1/chat/completions` with streaming).
- ContextBuilder stage: assembles prompt with citation markers, token
  budget enforced, raw chunk text from Postgres hydration.
- Per-tenant concurrency semaphore for LLM calls in Redis (`lr:llm:{cid}`).
- `AskQuestion` use case wires the full pipeline:
  `QdrantFilterBuilder → VectorRetriever → SectionExpansion →
  EvidenceGate → Reranker → ContextBuilder → LlmProvider →
  AnswerFinalizer (audit, citation, persistence)`.
- Drogon HTTP controller for `POST /v1/chat/turns` with SSE streaming
  of partial completion + final citation list.
- `chat_turns` persistence in same UoW as audit + outbox event for
  rag_sources indexing.
- **Critical:** Postgres connection released before LLM streaming
  begins; new connection acquired for persistence after stream completes
  (per Sonnet Obj. 3).
- Cancellation safety: client disconnect → cancel signal → semaphore
  released via RAII, partial chat_turn persisted as `status='cancelled'`.
- Per-tenant LLM rate limit (token-bucket in Redis).
- Curated answer stage stub (no terminal handling yet; returns
  empty).

### Exit criteria

- The chat endpoint streams an answer with citations for the test
  corpus's golden questions.
- A 100-concurrent-session load test runs cleanly: no executor
  starvation, no connection pool exhaustion, no Redis semaphore leaks.
- Cancellation tested: client disconnects mid-stream; nothing leaks.
- Audit + chat_turn + rag_sources_outbox-event are all written in
  one transaction.

### Test session #3: end-to-end chat

| Test | What it proves |
|---|---|
| Golden Q&A through full pipeline | End-to-end answers cite real chunks from in-scope documents |
| Citation correctness | Every citation in the response maps to a real chunk that was in the principal's scope at the moment the answer was produced |
| Cancellation safety (100 sessions, 50% kill rate) | Client disconnects do not leak PG connections, Redis semaphores, or LLM-provider tokens |
| Load: 100 concurrent chat sessions | p99 latency < target; no executor starvation; no pool exhaustion |
| Cross-tenant fuzz (10000 random requests across 3 tenants) | No response ever contains a chunk owned by another tenant |
| Rate limit honoured | Per-tenant token budget enforced; 11th concurrent request for a 10-cap tenant rejected with 429 |
| Audit completeness | Every chat turn that committed has matching audit_log and rag_sources rows; counts match exactly |
| Streaming vs non-streaming parity | Same Q&A in both modes returns the same final answer + citation list |

---

## Iteration 4 — Wiki publishing and operational polish

**Duration estimate:** 1.5 weeks

**Scope:** the second user-visible product surface + the operations
needed before any pilot customer can use it.

- `PublishWikiPage` use case: takes draft → creates new
  `wiki_page_version`, deprecates previous via the same pattern as
  document versions, persists `wiki_page_sources` pinned to specific
  `wiki_page_version_id` and `document_version_id` (per F2 fix).
- Wiki render endpoint + version-aware citation rendering.
- Wiki page diff between any two versions (compute server-side, cached
  in Redis).
- Admin endpoints: list audit log by company/action/timerange,
  grant/revoke memberships, promote/archive document versions.
- Metrics: Prometheus exporter on `/metrics`. Tenant-labeled business
  metrics; route-labeled latency metrics.
- Health endpoints: `/healthz` (liveness), `/readyz`
  (degraded-tolerant), `/admin/health` (deep, operator-only).
- Graceful shutdown: SIGTERM → stop accepting new requests → drain
  in-flight up to N seconds → release semaphores and leases → exit.
- Scheduler: advisory-lock-guarded periodic jobs (tombstone GC,
  review-due sweep, stuck-ingest re-enqueue, cache warmup).
- OpenAPI spec committed; CI validates handler signatures against spec.

### Exit criteria

- A wiki page can be published, referenced from another page, edited,
  and the diff between any two versions is correct.
- Killing any single process (api, ingest, scheduler) leaves the system
  in a recoverable state; restart resumes work.
- Rolling deploy simulation: ingest worker v1 receives a job written by
  api v2 with `job_schema_version=2` → worker rejects gracefully, job
  remains claimable by v2 worker after deploy completes.
- 24-hour soak test: 1 request/sec across all surfaces, no leaks, no
  unbounded queue growth, p99 stable.

### Test session #4: operational shakedown

| Test | What it proves |
|---|---|
| Wiki version round-trip | Publish v1, publish v2, query as-of past dates returns correct version |
| Wiki citation correctness | wiki_page_sources cite the correct (page_version, doc_version) pair after edits |
| Membership revoke during chat | A grant revoked mid-stream causes the next retrieval to honour the new scope |
| Move org_unit during chat | A concurrent `move_org_unit` operation does not produce stale scope for in-flight chats |
| Rolling deploy simulation | Version-aware job schema handling; mixed-version workers don't corrupt data |
| Each-process kill test | Kill api → other services healthy; kill ingest → jobs requeue; kill scheduler → recoverable on restart |
| 24h soak | No memory growth, no connection-pool leak, no Redis key explosion, p99 stable |
| OpenAPI conformance | Handler signatures match committed spec; no drift |

---

## Iteration 5 — Production readiness

**Duration estimate:** 2 weeks

**Scope:** what's needed before a paying customer's data touches the
system.

- OIDC integration (Keycloak reference deploy + bring-your-own option).
- `reactivate_user()` SQL function (V013) wired into auth callback.
- SCIM connector framework (port + Workday/Azure AD reference adapter).
- Optional: OpenTelemetry SDK adapter (real tracing).
- Optional: per-tenant cost accounting in `usage_events` table (V016).
- Eval harness (K7) wired up: replay golden questions on demand, store
  results in `eval_runs/eval_grades` tables.
- WASM plugin host stub (no plugin loading yet, just the port).
- Backup + restore documented and tested.
- Penetration test: explicit attempt to bypass tenant boundary, scope
  filter, sensitivity filter, and audit log via every endpoint.

### Exit criteria

- OIDC login works end-to-end with at least Keycloak.
- An eval baseline is captured; second eval run shows reproducible
  results (within rubric noise).
- Penetration test produces zero successful exploits.

### Test session #5: acceptance / pilot-customer dry-run

| Test | What it proves |
|---|---|
| OIDC sign-in (Keycloak) | First-time login creates user via `reactivate_user`; subsequent logins refresh PII |
| Deactivated user blocked | Auth layer rejects deactivated users at OIDC validation |
| Eval baseline | Golden question replay produces stable scores across two runs |
| Property test: 100k random sequences of grant/revoke/move/promote/archive | No invariant violation, no orphan rows, no consistency mismatch |
| Cross-tenant penetration test | Every endpoint × forged token combo produces 401/403, never data leak |
| Pilot scenario rehearsal | End-to-end: company signup → org tree setup → SCIM sync → document upload → ingest → wiki publish → chat → audit review |
| Backup + restore | Postgres dump + Qdrant snapshot can be restored to a fresh environment; smoke tests pass on the restored system |

---

## §10. The shared test corpus

All iterations 1–5 are measured against the same versioned corpus.
Stored in `tests/fixtures/corpus/` and loaded into the test environment
by a deterministic seeder.

**Shape:**

- **3 companies**, each with a distinct workload profile to surface
  different bug classes:
  - **Acme** (HR-heavy): policies, handbooks, leave forms,
    confidential terminations, contractor agreements.
  - **Beta** (legal-heavy): contracts, statutes, case law summaries,
    regulated communications.
  - **Gamma** (engineering): architecture docs, runbooks, postmortems,
    onboarding guides.
- **~50 documents total**, distributed across companies. Mix of file
  types (PDF, DOCX, HTML, MD, plain text) so parsers are exercised.
- **Multi-version documents**: at least 5 documents have v1 → v2 → v3
  history for testing historical retrieval and citation reproducibility.
- **Org tree of 3–5 levels per company**:
  - Acme: root → HR / Eng / Finance; HR → HR_Managers, HR_Generalists.
  - Beta: root → Legal / Operations; Legal → Litigation, Contracts.
  - Gamma: root → Platform / Product / Infra; Platform → Backend, Frontend.
- **Sensitivity-label spread**: ~70% internal, ~20% confidential
  (HR_Managers-only, Litigation-only), ~5% restricted (board-level),
  ~5% public.
- **Time-expiring memberships**: 2-3 contractor users with `expires_at`
  in the near future, for time-boxed access tests.
- **Deliberate "trap" documents**: at least 3 documents named like they
  belong to one scope but located in another (e.g., "HR_Handbook" in
  the Finance org_unit) to catch retrievers that match on text and
  ignore scope.

**Golden questions:**

- ~20 questions per company, each with:
  - Expected chunk IDs (the chunks that should be retrieved)
  - Expected citation count (often 1–3 for focused, 5+ for
    overview)
  - Expected answer markers (substrings that MUST appear or MUST NOT
    appear in the final answer)
  - The principal × scope combo the question is asked under
- ~5 cross-scope traps per company: questions a user shouldn't be
  able to answer because the relevant chunks are outside their scope.
  Expected behavior: the system says "no in-scope information found"
  rather than fabricating or leaking.

**Versioning:** the corpus has its own version number tracked in
`tests/fixtures/corpus/VERSION`. A new corpus version is a meaningful
change (added trap, expanded org tree, added sensitivity level). All
iterations' expected results are pinned to a specific corpus version.

---

## §11. Test infrastructure

### Smoke tests (already exist)

`db/smoke_test.sh` runs the migrations and verifies DB-level
invariants. Extended per iteration with new constraint tests.

### Integration tests (new in iteration 0)

`tests/integration/` runs full docker-compose stack:
- Postgres 17
- Redis 7
- Qdrant (latest stable)
- Mock llama-server (returns deterministic embeddings + reranker
  scores + completions from a fixture file, so tests are
  reproducible without real GPU)
- Wikore api + ingest + scheduler binaries from the current build

Each iteration's "test session" is a named integration-test target
that runs in CI on every PR touching the relevant module.

### Property tests (new in iteration 2)

`tests/property/` uses rapidcheck for invariants that cannot be
exercised exhaustively:
- Access scope monotonicity (grant → never sees less; revoke → never
  sees more after cache flush)
- EvidenceGate non-bypassability
- Closure consistency under arbitrary move/promote sequences
- Tenant boundary preservation

### Fault-injection tests (new in iteration 3)

`tests/fault_injection/` uses test doubles that fail at specific
points:
- Postgres commit succeeds, Qdrant returns 500 → outbox retries until
  success or DLQ
- Worker process killed mid-batch → restart resumes from outbox
- Redis times out → retrieval falls back to non-cached path
- LLM provider streams partial then errors → audit row records
  partial completion

### Eval harness (iteration 5)

`tests/eval/` replays the golden question set through the full
pipeline against a real LLM endpoint (configurable). Scores are
persisted to `eval_runs/eval_grades` and compared against the
established baseline. Run on demand, not every PR (LLM-cost-heavy).

---

## §12. CI gating

Per PR:
- Schema smoke tests (always)
- Unit tests (always)
- Integration tests for touched modules (always)
- Property tests for touched modules (always)
- Fault-injection tests for retrieval/persistence changes (changed only)

Per merge to main:
- Full integration suite
- Property tests at higher trial counts (e.g., 1000 → 10000)

Per release candidate:
- 24-hour soak test
- Penetration test re-run
- Eval harness against baseline

---

## §13. Risk register and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Drogon's PG client is less mature than libpqxx | medium | high | Iteration 0 concurrency spike; fall back to libpqxx if spike fails |
| llama-server reranker performance insufficient | medium | medium | Decouple; can swap to a separate reranker service over HTTP |
| bge-m3 embedding quality insufficient for legal/HR | low | medium | Eval harness in iteration 5 lets us swap models with a new Qdrant collection |
| Section parsing produces poor trees for some PDF formats | high | medium | Make `section_id` nullable (already done in V014); start without section expansion for unparseable docs |
| DOCX third-party library does not expose paragraph styles | resolved | - | Own parser via pugixml + miniz; ~300 lines, controls exactly which XML fields are read |
| Test corpus does not match real customer documents | medium | high | First pilot customer's corpus replaces the synthetic corpus after iteration 3 |
| Cancellation safety bugs leak resources | medium | medium | Aggressive RAII + fault-injection tests in iteration 3 |
| Outbox worker becomes a bottleneck under heavy invalidation | low | low | Horizontal scaling pattern; multiple scheduler instances via advisory locks |

---

## §14. What is NOT in this plan

Explicitly deferred to post-launch:

- WASM plugin loading (stub port in iteration 5; loader later)
- Multi-region deployment (architecture supports it; not built)
- Mobile app
- Slack/Teams bots
- Curated answer cards (K4; schema can be added later via migration)
- Per-tenant cost accounting (K7; schema in iteration 5 if asked)
- Real OpenTelemetry SDK (port abstraction in iteration 0, real
  adapter in iteration 5 or later)

---

— Author: Claude Opus 4.7
