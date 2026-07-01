#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/rag/vector_store.hpp"
#include "wikore/ingest/document_repo.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wikore::scheduler {

// ---------------------------------------------------------------------------
// ResyncWorker - drains qdrant_resync_chunk_acl events from outbox_events
// (V015), enqueued by V032's ACL-change triggers (grant create/revoke/expiry,
// document owner change, org-tree move) and by the schema-v3 backfill.
//
// Unlike OutboxWorker (which re-embeds and full-upserts on ingest), this worker
// NEVER re-embeds: an ACL change leaves the chunk text (and therefore its
// vector) unchanged. It recomputes the resource-axis scope live from Postgres
// (DocumentRepoPort::fetch_access_scopes) and rewrites only the ACL-relevant
// Qdrant payload keys (access_scope_ids, owner, sensitivity, lifecycle,
// acl_version) via VectorStorePort::set_payload.
//
// Correctness under out-of-order / multi-instance processing (design 2.3):
//   documents.qdrant_synced_version is the Postgres-side monotonic truth about
//   what Qdrant currently advertises. The worker:
//     1. reads documents.acl_version (v_cur);
//     2. if the event's acl_version < v_cur, drops it as SUPERSEDED (a newer
//        event, already enqueued by the later bump, will do the write);
//     3. recomputes scope, refreshes the column, set_payloads Qdrant with
//        acl_version = v_cur;
//     4. compare-and-sets: UPDATE ... SET qdrant_synced_version = v_cur
//        WHERE acl_version = v_cur. Zero rows means the version moved during
//        the Qdrant write (a concurrent bump raced in); the event is dropped
//        (its newer sibling covers the corpus) and synced_version is NOT
//        advanced past what Qdrant actually reflects.
//
// The gate (G1) never trusts acl_version for authorization -- a transient
// stale payload between racing writers is a recall issue, not a leak.
// ---------------------------------------------------------------------------

class ResyncWorker {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500);
        int max_attempts                        = 5;
        int batch_size                          = 16;
        // Stale-claim reaper lease: an event claimed longer than this is
        // presumed abandoned (worker crashed) and is reclaimed on the next
        // poll. Must exceed the worst-case per-event processing time.
        std::chrono::minutes claim_lease        = std::chrono::minutes(10);
    };

    ResyncWorker(drogon::orm::DbClientPtr                  db,
                 std::shared_ptr<rag::VectorStorePort>     vector_store,
                 std::shared_ptr<ingest::DocumentRepoPort> repo,
                 ShutdownPredicate                         shutdown_requested,
                 Options                                   opts);

    drogon::Task<void> run();
    drogon::Task<int>  drain_once();

    drogon::Task<int>  release_my_claims();
    drogon::Task<int>  reap_stale_claims();

    std::size_t events_completed()  const { return events_completed_.load(); }
    std::size_t events_failed()     const { return events_failed_.load(); }
    std::size_t events_superseded() const { return events_superseded_.load(); }

private:
    struct ClaimedEvent {
        std::string  id;
        std::string  company_id;
        std::string  document_id;   // aggregate_id
        std::int64_t acl_version = 0;
    };

    // Result of processing one event: distinguishes a superseded drop (also a
    // completion, but counted separately) from a genuine write completion.
    enum class Outcome { Completed, Superseded };

    drogon::Task<std::vector<ClaimedEvent>> claim_batch();
    drogon::Task<Result<Outcome>>           process(const ClaimedEvent& ev);
    drogon::Task<void>                      mark_completed(const std::string& event_id);
    drogon::Task<void>                      mark_failed(const std::string& event_id,
                                                        std::string_view  reason);

    drogon::orm::DbClientPtr                  db_;
    std::shared_ptr<rag::VectorStorePort>     vector_store_;
    std::shared_ptr<ingest::DocumentRepoPort> repo_;
    ShutdownPredicate                         shutdown_;
    Options                                   opts_;
    std::string                               worker_id_;
    std::atomic<std::size_t>                  events_completed_{0};
    std::atomic<std::size_t>                  events_failed_{0};
    std::atomic<std::size_t>                  events_superseded_{0};
};

} // namespace wikore::scheduler
