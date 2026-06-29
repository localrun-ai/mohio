#include "wikore/scheduler/outbox_worker.hpp"
#include "wikore/adapters/postgres/error_mapper.hpp"
#include "wikore/rag/types.hpp"
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <format>
#include <unistd.h>

namespace wikore::scheduler {

namespace {

// Same cancellable sleep helper used by the ingest worker (kept private
// to each translation unit to avoid a public dependency surface).
drogon::Task<void> co_sleep(std::chrono::milliseconds d)
{
    auto* loop = drogon::app().getLoop();
    struct Awaiter {
        trantor::EventLoop*           loop;
        std::chrono::milliseconds     delay;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) {
            loop->runAfter(static_cast<double>(delay.count()) / 1000.0,
                           [h]() mutable { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    co_await Awaiter{loop, d};
}

} // namespace

OutboxWorker::OutboxWorker(drogon::orm::DbClientPtr              db,
                            std::shared_ptr<rag::EmbedderPort>    embedder,
                            std::shared_ptr<rag::VectorStorePort> vector_store,
                            ShutdownPredicate                     shutdown_requested,
                            Options                               opts)
    : db_(std::move(db))
    , embedder_(std::move(embedder))
    , vector_store_(std::move(vector_store))
    , shutdown_(std::move(shutdown_requested))
    , opts_(std::move(opts))
{
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    worker_id_ = std::format("{}:{}", host, getpid());
}

// ---------------------------------------------------------------------------
// Claim a small batch of pending qdrant_upsert_chunk_payload events.
//
// Uses FOR UPDATE SKIP LOCKED so multiple scheduler instances can claim
// distinct events in parallel. Reaps stale claims (claimed_at older than
// claim_lease) before each poll so crashed workers don't strand events.
// ---------------------------------------------------------------------------

drogon::Task<std::vector<OutboxWorker::ClaimedEvent>> OutboxWorker::claim_batch()
{
    // Stale-claim reaper runs as a quick pre-pass: events claimed too long
    // ago belong to a crashed worker and should be reclaimable. We don't
    // strand them forever and we don't need a separate sweep.
    co_await reap_stale_claims();

    // Model-routing predicate: when this scheduler instance is configured
    // for a specific model, only claim events whose payload names that
    // model. Events for other models stay unclaimed (claimed_at IS NULL,
    // attempt_count unchanged) so the scheduler bound to the correct
    // model can claim them on its next poll. Without this filter, the
    // wrong-model worker would claim-then-fail in a tight loop and
    // exhaust the retry budget before the correct worker ever sees it.
    //
    // Empty expected_embed_model disables the filter (single-model
    // deployments that trust producers).
    constexpr auto kSql = R"(
        UPDATE outbox_events e
        SET    claimed_at    = now(),
               claimed_by    = $3,
               attempt_count = e.attempt_count + 1
        WHERE  e.id IN (
            SELECT id
            FROM   outbox_events
            WHERE  job_type        = 'qdrant_upsert_chunk_payload'
              AND  completed_at    IS NULL
              AND  claimed_at      IS NULL
              AND  attempt_count   < $2::int
              AND  next_attempt_at <= now()
              AND  ($4 = '' OR COALESCE(payload->>'embed_model_id', '') = $4)
            ORDER BY next_attempt_at
            FOR UPDATE SKIP LOCKED
            LIMIT $1::int
        )
        RETURNING
               e.id::text                                AS id,
               e.company_id::text                        AS company_id,
               e.aggregate_id::text                      AS aggregate_id,
               COALESCE(e.payload->>'document_id', '')   AS document_id,
               COALESCE(e.payload->>'embed_model_id', '') AS embed_model_id
    )";

    std::vector<ClaimedEvent> events;
    try {
        auto rows = co_await db_->execSqlCoro(kSql,
            opts_.batch_size,
            opts_.max_attempts,
            worker_id_,
            opts_.expected_embed_model);
        events.reserve(rows.size());
        for (const auto& r : rows) {
            events.push_back(ClaimedEvent{
                .id             = r["id"].as<std::string>(),
                .company_id     = r["company_id"].as<std::string>(),
                .aggregate_id   = r["aggregate_id"].as<std::string>(),
                .document_id    = r["document_id"].as<std::string>(),
                .embed_model_id = r["embed_model_id"].as<std::string>(),
            });
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[outbox-worker] claim_batch failed: {}", ex.base().what());
    }
    co_return events;
}

drogon::Task<int> OutboxWorker::reap_stale_claims()
{
    // Events claimed too long ago belong to a crashed worker. Release
    // them back to the unclaimed pool. Decrement attempt_count so the
    // retry budget isn't consumed by abandoned attempts.
    try {
        auto rows = co_await db_->execSqlCoro(std::format(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0),
                   last_error    = COALESCE(last_error, '') ||
                                   ' [reaped: stale claim by ' ||
                                   COALESCE(claimed_by, '?') || ']'
            WHERE  job_type     = 'qdrant_upsert_chunk_payload'
              AND  completed_at IS NULL
              AND  claimed_at IS NOT NULL
              AND  claimed_at < now() - interval '{} minutes'
            RETURNING id::text
        )", opts_.claim_lease.count()));
        const int n = static_cast<int>(rows.size());
        if (n > 0) {
            for (const auto& r : rows) {
                spdlog::warn("[outbox-worker] reaped stale claim on event {} "
                             "(claim age > {} min)",
                             r["id"].as<std::string>(),
                             opts_.claim_lease.count());
            }
        }
        co_return n;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[outbox-worker] reap_stale_claims failed: {}",
                     ex.base().what());
        co_return 0;
    }
}

drogon::Task<int> OutboxWorker::release_my_claims()
{
    // Called on graceful shutdown: release events still claimed by this
    // worker_id, decrementing attempt_count so a clean shutdown doesn't
    // burn the retry budget.
    try {
        auto rows = co_await db_->execSqlCoro(R"(
            UPDATE outbox_events
            SET    claimed_at    = NULL,
                   claimed_by    = NULL,
                   attempt_count = GREATEST(attempt_count - 1, 0)
            WHERE  job_type     = 'qdrant_upsert_chunk_payload'
              AND  completed_at IS NULL
              AND  claimed_by   = $1
            RETURNING id::text
        )", worker_id_);
        const int n = static_cast<int>(rows.size());
        if (n > 0)
            spdlog::info("[outbox-worker] {} released {} unprocessed claim(s) on shutdown",
                         worker_id_, n);
        co_return n;
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::warn("[outbox-worker] release_my_claims failed: {}",
                     ex.base().what());
        co_return 0;
    }
}

// ---------------------------------------------------------------------------
// Process one claimed event: read chunks -> embed -> upsert to vector store
// -> write document_chunk_vectors rows -> mark completed.
//
// On any step's failure, the event remains uncompleted (claimed_at cleared
// in mark_failed) so the next poll re-attempts.
// ---------------------------------------------------------------------------

drogon::Task<Result<void>> OutboxWorker::process(const ClaimedEvent& ev)
{
    // 0. Model routing: this scheduler instance is configured to serve a
    //    single embedding model + Qdrant collection. Reject events naming
    //    a different model so a payload tagged 'bge-m3' cannot be embedded
    //    with text-embedding-3-small and silently stored as 'bge-m3'.
    //    Empty expected_embed_model disables the check (single-model
    //    deployments that trust producers).
    if (!opts_.expected_embed_model.empty()
        && ev.embed_model_id != opts_.expected_embed_model)
    {
        co_return std::unexpected(Error::invalid_state(std::format(
            "outbox: event embed_model_id={} does not match this scheduler's "
            "configured model={}; route to a scheduler instance bound to "
            "the correct model",
            ev.embed_model_id, opts_.expected_embed_model)));
    }

    // 1. Look up the embedding model row so we can write document_chunk_vectors.
    //    A missing model is a hard error -- the producer wrote an event for
    //    a model name that doesn't exist in our registry.
    std::string embedding_model_id;
    try {
        auto rows = co_await db_->execSqlCoro(
            "SELECT id::text FROM embedding_models WHERE name = $1",
            ev.embed_model_id);
        if (rows.empty()) {
            co_return std::unexpected(Error::invalid_state(std::format(
                "outbox: embed_model_id={} not in embedding_models registry",
                ev.embed_model_id)));
        }
        embedding_model_id = rows[0]["id"].as<std::string>();
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    // 2. Pull the chunks from Postgres. Per the architecture invariant
    //    ("Qdrant is index, Postgres is evidence"), the chunk content
    //    always comes from PG, never from a previous Qdrant payload.
    struct DbChunk {
        std::string                chunk_id;
        int                        chunk_index = 0;
        std::string                content;
        std::vector<std::string>   access_scope_ids;
        std::optional<std::string> section_id;
        std::optional<std::string> section_heading;
        std::string                sensitivity_label;
        std::string                lifecycle_status;
        std::optional<std::string> activated_at;
        std::optional<std::string> superseded_at;
        std::string                owner_org_unit_id;   // documents.owner_org_unit_id
        int                        authority_level = 50; // documents.authority_level
    };
    std::vector<DbChunk> chunks;
    try {
        // V014 added document_sections; join on it for the section_heading.
        // V003: document_chunks(company_id, document_version_id, chunk_index,
        //                       content, access_scope_ids, section_id).
        // V014: document_versions(sensitivity_label, lifecycle_status,
        //                          activated_at, superseded_at).
        // V003: documents(owner_org_unit_id, authority_level): reranker
        //       weight + canonical owning OU for the payload contract.
        auto rows = co_await db_->execSqlCoro(R"(
            SELECT  dc.id::text                              AS chunk_id,
                    dc.chunk_index                           AS chunk_index,
                    dc.content                               AS content,
                    dc.section_id::text                      AS section_id,
                    ds.heading                               AS section_heading,
                    array_to_string(dc.access_scope_ids, ',') AS access_scope_csv,
                    dv.sensitivity_label                     AS sensitivity_label,
                    dv.lifecycle_status                      AS lifecycle_status,
                    dv.activated_at                          AS activated_at,
                    dv.superseded_at                         AS superseded_at,
                    d.owner_org_unit_id::text                AS owner_org_unit_id,
                    d.authority_level                        AS authority_level
            FROM    document_chunks  dc
            JOIN    document_versions dv ON dv.id = dc.document_version_id
            JOIN    documents         d  ON d.id  = dv.document_id
            LEFT JOIN document_sections ds ON ds.id = dc.section_id
            WHERE   dc.document_version_id = $1::uuid
              AND   dc.company_id          = $2::uuid
            ORDER BY dc.chunk_index
        )", ev.aggregate_id, ev.company_id);

        chunks.reserve(rows.size());
        for (const auto& r : rows) {
            DbChunk c;
            c.chunk_id           = r["chunk_id"].as<std::string>();
            c.chunk_index        = r["chunk_index"].as<int>();
            c.content            = r["content"].as<std::string>();
            c.sensitivity_label  = r["sensitivity_label"].as<std::string>();
            c.lifecycle_status   = r["lifecycle_status"].as<std::string>();
            c.owner_org_unit_id  = r["owner_org_unit_id"].as<std::string>();
            c.authority_level    = r["authority_level"].as<int>();
            if (!r["section_id"].isNull())
                c.section_id = r["section_id"].as<std::string>();
            if (!r["section_heading"].isNull())
                c.section_heading = r["section_heading"].as<std::string>();
            if (!r["activated_at"].isNull())
                c.activated_at = r["activated_at"].as<std::string>();
            if (!r["superseded_at"].isNull())
                c.superseded_at = r["superseded_at"].as<std::string>();
            // CSV split for access_scope_ids; the column is UUID[].
            const auto csv = r["access_scope_csv"].as<std::string>();
            std::size_t pos = 0;
            while (pos < csv.size()) {
                auto comma = csv.find(',', pos);
                auto end   = (comma == std::string::npos) ? csv.size() : comma;
                if (end > pos)
                    c.access_scope_ids.emplace_back(csv.substr(pos, end - pos));
                pos = (comma == std::string::npos) ? csv.size() : comma + 1;
            }
            chunks.push_back(std::move(c));
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    if (chunks.empty()) {
        // No chunks for this version -- legal if the version has zero
        // content (e.g. an empty document). Nothing to embed; the event
        // is still considered complete.
        spdlog::info("[outbox-worker] event={} version={} has zero chunks; marking complete",
                     ev.id, ev.aggregate_id);
        co_return Result<void>{};
    }

    // 3. Embed in batches.
    std::vector<rag::UpsertPoint> points;
    points.reserve(chunks.size());
    for (std::size_t i = 0; i < chunks.size(); i += static_cast<std::size_t>(opts_.embed_batch_size)) {
        const auto end =
            std::min(i + static_cast<std::size_t>(opts_.embed_batch_size), chunks.size());
        std::vector<std::string> texts;
        texts.reserve(end - i);
        for (std::size_t j = i; j < end; ++j)
            texts.push_back(chunks[j].content);

        auto embed_result = co_await embedder_->embed_batch(std::move(texts));
        if (!embed_result)
            co_return std::unexpected(embed_result.error());
        const auto& vectors = *embed_result;

        for (std::size_t j = 0; j < vectors.size(); ++j) {
            const auto& chunk = chunks[i + j];
            rag::ChunkPayload payload{
                .company_id          = ev.company_id,
                .document_id         = ev.document_id,
                .document_version_id = ev.aggregate_id,
                .owner_org_unit_id   = chunk.owner_org_unit_id,
                .chunk_id            = chunk.chunk_id,
                .chunk_index         = chunk.chunk_index,
                .authority_level     = chunk.authority_level,
                .access_scope_ids    = chunk.access_scope_ids,
                .sensitivity_label   = chunk.sensitivity_label,
                .lifecycle_status    = chunk.lifecycle_status,
                .activated_at        = chunk.activated_at,
                .superseded_at       = chunk.superseded_at,
                .section_id          = chunk.section_id,
                .section_heading     = chunk.section_heading,
            };
            const auto point_id = rag::uuid_v5(
                chunk.chunk_id + ":" + ev.embed_model_id);
            points.push_back({point_id, vectors[j], std::move(payload)});
        }
    }

    // 4. Upsert to the vector store.
    if (auto r = co_await vector_store_->upsert(points); !r)
        co_return std::unexpected(r.error());

    // 5. document_chunk_vectors bookkeeping: one row per (chunk, model).
    //    ON CONFLICT DO NOTHING because deterministic point IDs mean a
    //    re-run already has a matching row.
    //
    //    Bookkeeping is part of the consistency contract -- callers join
    //    chunks -> vectors -> points to find the Qdrant point for a chunk.
    //    A bookkeeping failure must fail the event so the next poll
    //    retries. Qdrant upsert is idempotent on the deterministic point
    //    IDs, so a retry just no-ops the actual vector write.
    try {
        for (const auto& point : points) {
            co_await db_->execSqlCoro(R"(
                INSERT INTO document_chunk_vectors
                       (company_id, chunk_id, embedding_model_id, qdrant_point_id)
                VALUES ($1::uuid, $2::uuid, $3::uuid, $4::uuid)
                ON CONFLICT (chunk_id, embedding_model_id) DO NOTHING
            )", ev.company_id, point.payload.chunk_id, embedding_model_id, point.id);
        }
    } catch (const drogon::orm::DrogonDbException& ex) {
        co_return std::unexpected(postgres::map_db_exception(ex));
    }

    co_return Result<void>{};
}

drogon::Task<void> OutboxWorker::mark_completed(const std::string& event_id)
{
    try {
        co_await db_->execSqlCoro(
            "UPDATE outbox_events SET completed_at = now(), claimed_at = NULL "
            "WHERE id = $1::uuid",
            event_id);
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[outbox-worker] mark_completed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<void> OutboxWorker::mark_failed(const std::string& event_id,
                                              std::string_view  reason)
{
    try {
        // Clear claimed_at so the next poll can re-attempt (attempt_count
        // already incremented at claim time). Schedule the next attempt
        // with exponential backoff so a transient downstream outage
        // (Qdrant blip, llama-server timeout) doesn't burn the retry
        // budget in seconds: 2s, 4s, 8s, 16s, 32s, capped at 300s.
        //
        // Cap is bounded so a long outage never strands events
        // indefinitely; we just poll once per cap interval after
        // recovery instead of hammering during it.
        co_await db_->execSqlCoro(R"(
            UPDATE outbox_events
            SET    claimed_at      = NULL,
                   claimed_by      = NULL,
                   last_error      = $2,
                   next_attempt_at = now()
                                   + (LEAST(300, POWER(2, attempt_count)::int)
                                      * interval '1 second')
            WHERE  id = $1::uuid
        )", event_id, std::string(reason));
    } catch (const drogon::orm::DrogonDbException& ex) {
        spdlog::error("[outbox-worker] mark_failed({}) failed: {}",
                      event_id, ex.base().what());
    }
}

drogon::Task<int> OutboxWorker::drain_once()
{
    auto batch = co_await claim_batch();
    int completed = 0;
    for (const auto& ev : batch) {
        if (shutdown_()) break;
        auto r = co_await process(ev);
        if (r) {
            co_await mark_completed(ev.id);
            events_completed_.fetch_add(1);
            ++completed;
        } else {
            const auto& err = r.error();
            co_await mark_failed(ev.id, err.message);
            events_failed_.fetch_add(1);
            spdlog::warn("[outbox-worker] event={} version={} failed: {}",
                         ev.id, ev.aggregate_id, err.message);
        }
    }
    co_return completed;
}

drogon::Task<void> OutboxWorker::run()
{
    spdlog::info("[outbox-worker] {} starting; poll={}ms batch_size={} lease={}min model={}",
                 worker_id_, opts_.poll_interval.count(), opts_.batch_size,
                 opts_.claim_lease.count(),
                 opts_.expected_embed_model.empty() ? "<any>" : opts_.expected_embed_model);
    while (!shutdown_()) {
        int completed = co_await drain_once();
        if (completed < opts_.batch_size) {
            co_await co_sleep(opts_.poll_interval);
        }
    }
    // Release any events still claimed by this worker so a clean shutdown
    // doesn't strand them (claimed_at would otherwise stay non-NULL and
    // claim_batch's WHERE filter would skip them until the lease expires).
    co_await release_my_claims();
    spdlog::info("[outbox-worker] {} drained; exiting", worker_id_);
}

} // namespace wikore::scheduler