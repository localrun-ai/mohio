#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/rag/embedder.hpp"
#include "wikore/rag/vector_store.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace wikore::scheduler {

// ---------------------------------------------------------------------------
// OutboxWorker - drains qdrant_upsert_chunk_payload events from
// outbox_events (V015). See implementation file for the full pipeline.
// ---------------------------------------------------------------------------

class OutboxWorker {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500);
        int max_attempts                        = 5;
        int batch_size                          = 16;
        int embed_batch_size                    = 32;
        // Reaper for stale claims: an event with claimed_at older than
        // this is considered abandoned (worker crashed) and is reclaimed
        // by the next poll. Must be longer than any legitimate event's
        // processing time so a slow embedding pass doesn't get stolen.
        std::chrono::minutes claim_lease        = std::chrono::minutes(10);
        // The model this scheduler instance is configured to serve. Events
        // whose payload's embed_model_id does not match are marked failed
        // with a clear error. Empty string disables the check (single-model
        // deployment that trusts producers).
        std::string expected_embed_model;
    };

    OutboxWorker(drogon::orm::DbClientPtr              db,
                 std::shared_ptr<rag::EmbedderPort>    embedder,
                 std::shared_ptr<rag::VectorStorePort> vector_store,
                 ShutdownPredicate                     shutdown_requested,
                 Options                               opts);

    drogon::Task<void> run();
    drogon::Task<int>  drain_once();

    // Releases any events still claimed by THIS worker_id back to the
    // unclaimed pool, decrementing attempt_count so the retry budget
    // is not consumed by a graceful shutdown. Called automatically at
    // the end of run() (after shutdown_() flips true), and also exposed
    // for tests that want to assert release behaviour explicitly.
    drogon::Task<int>  release_my_claims();

    // Reclaims events whose claimed_at is older than opts_.claim_lease;
    // these are presumed to belong to a crashed worker. Decrements
    // attempt_count so a slow embedder doesn't burn the retry budget.
    // Idempotent; returns the number of events reclaimed.
    drogon::Task<int>  reap_stale_claims();

    std::size_t events_completed() const { return events_completed_.load(); }
    std::size_t events_failed()    const { return events_failed_.load(); }

private:
    struct ClaimedEvent {
        std::string id;
        std::string company_id;
        std::string aggregate_id;
        std::string document_id;
        std::string embed_model_id;
    };

    drogon::Task<std::vector<ClaimedEvent>> claim_batch();
    drogon::Task<Result<void>>              process(const ClaimedEvent& ev);
    drogon::Task<void>                      mark_completed(const std::string& event_id);
    drogon::Task<void>                      mark_failed(const std::string& event_id,
                                                         std::string_view  reason);

    drogon::orm::DbClientPtr              db_;
    std::shared_ptr<rag::EmbedderPort>    embedder_;
    std::shared_ptr<rag::VectorStorePort> vector_store_;
    ShutdownPredicate                     shutdown_;
    Options                               opts_;
    std::string                           worker_id_;
    std::atomic<std::size_t>              events_completed_{0};
    std::atomic<std::size_t>              events_failed_{0};
};

} // namespace wikore::scheduler