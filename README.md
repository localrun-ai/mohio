# Wikore

Wikore is a self-hosted, permission-aware enterprise wiki and
retrieval-augmented generation (RAG) backend. It is designed for organizations
that need document retrieval to respect tenant, organizational, lifecycle, and
sensitivity boundaries.

The project is written in C++23 with Drogon. PostgreSQL is the authoritative
store, Qdrant is a rebuildable vector index, and Redis provides queues and
short-lived coordination.

> **Project status:** active development, not production-ready. The Iteration 1
> ingestion, transactional outbox, vector-indexing worker, and crash-recovery
> foundation are implemented. Most HTTP routes, complete permission resolution
> and ACL resynchronization, non-text parsers, the chat pipeline, wiki features,
> and the evaluation CLI are still under development.

## Architecture

```mermaid
flowchart LR
    Client[API clients] --> API[wikore_api]
    API --> PG[(PostgreSQL<br/>authoritative state)]
    API -. planned upload enqueue .-> Queue[(Redis<br/>tenant ingest queues)]

    Queue --> Ingest[wikore_ingest]
    Ingest -->|chunks + audit + outbox<br/>one transaction| PG

    PG -->|claim outbox events| Scheduler[wikore_scheduler]
    Scheduler --> Embed[OpenAI-compatible<br/>embedding endpoint]
    Scheduler --> Qdrant[(Qdrant<br/>derived vector index)]
    Scheduler -->|vector bookkeeping| PG

    Scheduler <-->|heartbeats, recovery queues| Queue

    API -. planned retrieval .-> Embed
    API -. filtered candidates .-> Qdrant
    API -. evidence hydration .-> PG
    API -. grounded prompt .-> LLM[OpenAI-compatible<br/>LLM endpoint]
```

Embedding and Qdrant writes do not occur inside the ingest transaction. The
ingest worker commits the durable document state and an outbox event first. The
scheduler later embeds the committed chunks, upserts deterministic Qdrant
points, and records the chunk-to-vector mapping in PostgreSQL.

### Components

| Binary | Responsibility | Current status |
| --- | --- | --- |
| `wikore_api` | Authenticated document, organization, wiki, chat, and administration API | Health route works; most application routes return `501` |
| `wikore_ingest` | Fair per-tenant queue consumption, parsing, chunking, and transactional persistence | Implemented for plain text and Markdown |
| `wikore_scheduler` | Outbox draining, embedding, Qdrant indexing, stale-claim recovery, and ingest recovery sweeps | Iteration 1 path implemented |
| `wikore_eval` | Retrieval and answer-quality evaluation harness | Startup stub; planned for a later iteration |

The source is organized as a modular monolith, while expensive and scheduled
work runs in separate processes. The core libraries can also be exercised with
in-memory test adapters.

## Design Invariants

- **PostgreSQL is authoritative.** Chunk content, lifecycle state, audit data,
  outbox events, and vector bookkeeping live in PostgreSQL. Redis and Qdrant are
  treated as derived or transient stores.
- **Tenant boundaries are database-enforced.** Cross-entity relationships use
  tenant-scoped composite foreign keys where possible, rather than relying only
  on application checks.
- **External side effects are asynchronous.** The ingest use case writes chunks,
  audit data, terminal ingest state, and the outbox event in one transaction.
- **Retries are idempotent.** Chunk rows use stable conflict keys and Qdrant
  points use deterministic UUIDs derived from the chunk and embedding model.
- **Worker ownership is explicit.** Ingest claims use per-claim tokens so a
  stale worker cannot overwrite a newer worker's terminal result.
- **Crashes remain recoverable.** Redis processing lists, worker heartbeats,
  persisted ingest payloads, bounded retry counters, stale outbox claims, and
  polling sweeps cover the worker-crash windows implemented in Iteration 1.
- **Retrieval must fail closed.** The retrieval adapters return no candidates
  when the resolved access-scope filter is empty. The complete grant model and
  resynchronization path are still being finalized before retrieval routes ship.

## Ingestion Flow

1. A document version is persisted as `pending` and its job is placed on a
   tenant-specific Redis queue.
2. `wikore_ingest` rotates fairly across discovered tenant queues and moves one
   job to a worker-specific processing list.
3. A compare-and-set transition claims the version as `processing`, persists
   the recovery payload, and creates an ownership token.
4. The worker validates and parses the file, builds a section tree, creates
   section-aware overlapping chunks, and resolves the chunk access snapshot.
5. Sections, chunks, an audit row, the outbox event, and `ingest_status='done'`
   commit atomically in PostgreSQL.
6. `wikore_scheduler` claims the outbox event with `FOR UPDATE SKIP LOCKED`,
   validates the configured embedding model against the model registry, embeds
   the chunks in batches, and upserts them into the model's Qdrant collection.
7. The scheduler records `document_chunk_vectors` rows and marks the outbox
   event complete. Transient failures use persisted exponential backoff.

On shutdown, workers drain in-flight work before exiting. If an ingest worker
dies, the scheduler can recover its processing list or requeue a stale database
claim. Poison jobs have a bounded resume budget and eventually transition to
`error` rather than looping forever.

## Build

Wikore currently targets Linux. CI builds on Ubuntu 24.04 with GCC and CMake
3.28 or newer.

Install the system dependencies used by CI:

```sh
sudo apt-get update
sudo apt-get install cmake ninja-build ccache libspdlog-dev libssl-dev \
  libhiredis-dev libjsoncpp-dev libpq-dev uuid-dev git
```

Configure and build:

```sh
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWIKORE_NATIVE_OPTS=OFF
cmake --build build --parallel
```

CMake fetches Drogon, Glaze, jwt-cpp, and Catch2 during configuration. Debug
builds enable AddressSanitizer, UndefinedBehaviorSanitizer, and warnings as
errors.

Useful CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `WIKORE_BUILD_APPS` | `ON` | Build all four executables |
| `WIKORE_BUILD_TESTS` | `ON` | Build the Catch2 test executable |
| `WIKORE_NATIVE_OPTS` | `ON` | Enable native CPU and link-time optimizations in Release builds |

## Database Setup

Wikore requires PostgreSQL 17. It does not yet include an application-level
migration runner, so apply the SQL migrations before starting the binaries:

```sh
export DATABASE_URL=postgresql://wikore:wikore@localhost:5432/wikore
ls db/migrations/V*.sql | sort | xargs cat \
  | psql "$DATABASE_URL" -v ON_ERROR_STOP=1
```

The scheduler also requires an enabled embedding-model registry row whose name
and dimension match `EMBED_MODEL` and `EMBED_DIMS`:

```sql
INSERT INTO embedding_models (name, qdrant_collection, dimension)
VALUES ('bge-m3', 'wikore_docs_bge_m3', 1024);
```

Use the values exposed by your embedding server. The scheduler fails fast with
a non-zero exit status if the model is missing or disabled, dimensions differ,
or the Qdrant collection cannot be initialized.

For the example above, export matching runtime values before starting workers:

```sh
export EMBED_MODEL=bge-m3
export EMBED_DIMS=1024
```

## Configuration

Configuration is read from environment variables. The checked-in
[`.env.example`](.env.example) is a starting point.

| Variable | Default | Used for |
| --- | --- | --- |
| `DATABASE_URL` | `postgresql://wikore:wikore@localhost:5432/wikore` | PostgreSQL connection |
| `REDIS_URL` | `redis://127.0.0.1:6379/0` | Queues, heartbeats, and caches |
| `QDRANT_URL` | `http://localhost:6333` | Vector index |
| `EMBED_BASE_URL` | `http://localhost:8081/v1` | OpenAI-compatible embeddings endpoint |
| `EMBED_MODEL` | empty | Registry model name used by ingest and scheduler |
| `EMBED_DIMS` | `768` | Expected embedding dimension |
| `LLM_BASE_URL` | `http://localhost:8080/v1` | OpenAI-compatible generation endpoint |
| `LLM_MODEL` | empty | Generation model name |
| `LLM_MAX_TOKENS` | `2048` | Maximum generated tokens |
| `LLM_CONCURRENCY` | `4` | Planned global in-flight generation cap |
| `OIDC_ISSUER` | empty | OIDC issuer used to load and validate JWKS |
| `OIDC_AUDIENCE` | `wikore` | Expected token audience |
| `CREDENTIALS_KEY` | empty | 64-character hex AES-256-GCM key for integration secrets |
| `PORT` | `9000` | API listen port |
| `ALLOWED_ORIGIN` | `*` | Planned API CORS origin setting; wiring is incomplete |
| `ANTHROPIC_API_KEY` | empty | Optional future wiki-generation provider key |
| `ANTHROPIC_MODEL` | `claude-sonnet-4-6` | Optional future wiki-generation model |

Generate a credential-encryption key with:

```sh
openssl rand -hex 32
```

## Running Locally

After PostgreSQL, Redis, Qdrant, and the embedding endpoint are available and
the schema/model registry are initialized, start the background processes:

```sh
./build/apps/scheduler/wikore_scheduler
./build/apps/ingest/wikore_ingest
```

The API can be started separately:

```sh
./build/apps/api/wikore_api
curl http://localhost:9000/api/health
```

At the current stage, the health route is the only completed public HTTP flow;
ingest behavior is primarily exercised through integration tests and internal
use cases.

## Testing

```sh
# C++ unit tests; DB-backed cases skip when DATABASE_URL is absent
ctest --test-dir build --output-on-failure

# Load every migration into PostgreSQL 17 and verify schema behavior
make smoke

# Run PostgreSQL + Redis integration tests using local service containers
bash scripts/test_integration_local.sh

# Verify deterministic corpus tooling and generated negative samples
make corpus-test corpus-verify
```

CI runs three required jobs on every pull request:

1. PostgreSQL 17 schema smoke tests.
2. Release build and non-database Catch2 tests.
3. PostgreSQL and Redis integration tests using the compiled test artifact.

## Current Scope

Implemented foundation:

- PostgreSQL migrations through V030, including tenant constraints, lifecycle
  state, append-only records, temporal access history, outbox retries, and ingest
  ownership tokens.
- Hardened UTF-8 plain-text and Markdown parsing with size, MIME, binary,
  Unicode-tag, hidden-HTML, and EICAR checks.
- Section-aware chunking and transactional document persistence.
- Fair multi-tenant ingest consumption with graceful shutdown and crash recovery.
- Embedding-model registry validation, batched embeddings, deterministic Qdrant
  writes, and PostgreSQL vector bookkeeping.
- OIDC/JWT and API-key authentication foundations, Qdrant adapters, null test
  adapters, schema smoke tests, and integration coverage.

Not yet complete:

- Production document upload and administration routes.
- Final permission-set algebra, group-aware resolution, cache invalidation, and
  Qdrant ACL resynchronization.
- PDF, DOCX, HTML, and other rich-document parsers.
- End-to-end retrieval, evidence gating, reranking, answer generation, and SSE.
- Wiki operations, MCP integrations, retention jobs, partition maintenance, and
  the evaluation harness.
