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
//   what Qdrant currently advertises. Each event is processed inside ONE
//   transaction that first takes pg_advisory_xact_lock(hashtext('resync:doc:'
//   || document_id)), so no two workers can interleave read->write->CAS for
//   the same document. Without the lock, a slow OLDER write (v2) can land in
//   Qdrant AFTER a newer write (v3) and stick, while synced_version stays at 3
//   -- a durable lie. The lock serializes per-document resyncs so the last
//   Qdrant write always corresponds to the highest processed version. Under
//   the lock the worker:
//     1. reads documents.acl_version (v_cur);
//     2. if the event's acl_version < v_cur, drops it as SUPERSEDED (a newer
//        event, already enqueued by the later bump, will do the write);
//     3. recomputes scope, refreshes the column, and set_payloads EVERY Qdrant
//        collection the document has points in (per-model; see
//        VectorStoreForCollection) with acl_version = v_cur, using wait=true so
//        Qdrant has applied the change before the CAS commits;
//     4. compare-and-sets: UPDATE ... SET qdrant_synced_version = v_cur
//        WHERE acl_version = v_cur. Zero rows means a concurrent grant-mutation
//        bump moved the version during processing; the event is dropped (its
//        newer sibling, enqueued by that bump, re-writes under the same lock)
//        and synced_version is NOT advanced past what Qdrant reflects.
//
// The gate (G1) never trusts acl_version for authorization -- a transient
// stale payload is a recall issue, not a leak.
// ---------------------------------------------------------------------------

class ResyncWorker {
public:
    using ShutdownPredicate = std::function<bool()>;

    // Resolve the vector store bound to a given Qdrant collection. A chunk may
    // be embedded by several models (document_chunk_vectors is per-model), each
    // in its own collection (embedding_models.qdrant_collection); the worker
    // must patch EVERY collection the document has points in before advancing
    // the document-wide qdrant_synced_version, or other models' collections
    // would be left stale while PG advertises them as synced. Returns nullptr
    // for a collection this deployment does not serve (treated as a hard error
    // so the mismatch surfaces instead of silently skipping a collection).
    using VectorStoreForCollection =
        std::function<std::shared_ptr<rag::VectorStorePort>(const std::string& collection)>;

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
                 VectorStoreForCollection                  store_for_collection,
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
    VectorStoreForCollection                  store_for_collection_;
    std::shared_ptr<ingest::DocumentRepoPort> repo_;
    ShutdownPredicate                         shutdown_;
    Options                                   opts_;
    std::string                               worker_id_;
    std::atomic<std::size_t>                  events_completed_{0};
    std::atomic<std::size_t>                  events_failed_{0};
    std::atomic<std::size_t>                  events_superseded_{0};
};

} // namespace wikore::scheduler
