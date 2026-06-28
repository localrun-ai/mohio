#pragma once
#include "wikore/application/ingest_document_version.hpp"
#include "wikore/domain/types.hpp"
#include "wikore/ingest/types.hpp"
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// IngestWorker
//
// Consumes per-tenant Redis queues (lr:ingest:q:{company_id}) in round-robin
// order so no single tenant can starve the others, then dispatches each job
// to IngestDocumentVersionUseCase::execute().
//
// Fair-scheduling model:
//   1. Discover tenant queues via SCAN MATCH 'lr:ingest:q:*'. Result is
//      cached for `rescan_interval_` (default 30 s) and refreshed in the
//      idle gap, so new tenants are picked up without restart.
//   2. Each tick advances a per-instance cursor through the cached list,
//      attempting one non-blocking RPOP per tenant. The first non-empty
//      pop drives the dispatch. This gives strict round-robin under load.
//   3. If a full rotation yields zero jobs, sleep `idle_sleep_` (default
//      200 ms) before the next rotation. Configurable for tests.
//
// Shutdown:
//   The worker re-checks `shutdown_requested_()` at every rotation boundary
//   AND between each tenant probe, so SIGTERM drain time is bounded by the
//   length of a single job's `execute()` plus one probe round-trip.
// ---------------------------------------------------------------------------

class IngestWorker {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::milliseconds idle_sleep      = std::chrono::milliseconds(200);
        std::chrono::seconds      rescan_interval = std::chrono::seconds(30);
        std::chrono::seconds      deadline_per_job = std::chrono::seconds(600);
        // Heartbeat key TTL. Sweep reaper considers the worker dead when
        // the key expires, so heartbeat_ttl must exceed both the maximum
        // job duration and the worker's rotation period with margin.
        std::chrono::seconds      heartbeat_ttl    = std::chrono::seconds(900);
        // How often to refresh the heartbeat. Must be < heartbeat_ttl.
        std::chrono::seconds      heartbeat_period = std::chrono::seconds(30);
        // 'wikore-ingest-host-pid' style identifier; written to logs,
        // the Redis processing-list key, and the heartbeat key.
        std::string               worker_id;
    };

    IngestWorker(application::IngestDocumentVersionUseCase use_case,
                 ShutdownPredicate                          shutdown_requested,
                 Options                                    opts);

    // Long-running coroutine; returns when shutdown_requested_() becomes true.
    drogon::Task<void> run();

    // Test introspection.
    std::size_t jobs_processed()  const { return jobs_processed_.load(); }
    std::size_t jobs_failed()     const { return jobs_failed_.load(); }
    std::size_t jobs_duplicates() const { return jobs_duplicates_.load(); }

    // Test introspection / supervision: true when the worker has
    // entered a non-recoverable infra state (e.g. Redis-permanently-
    // unavailable mid-transfer-back). run() exits when this is set.
    bool fatal_failure() const { return fatal_failure_.load(); }
    const std::string& worker_id() const { return opts_.worker_id; }

private:
    drogon::Task<void> rescan_tenants_locked();
    drogon::Task<bool> try_one_rotation();    // returns true if any job dispatched
    drogon::Task<void> dispatch(IngestJob job, std::string payload, std::string proc_key);
    void               refresh_heartbeat();

    application::IngestDocumentVersionUseCase use_case_;
    ShutdownPredicate                          shutdown_;
    Options                                    opts_;

    std::vector<std::string>               tenants_;
    std::size_t                            cursor_     = 0;
    std::chrono::steady_clock::time_point  last_scan_  = {};

    std::atomic<std::size_t>               jobs_processed_{0};
    std::atomic<std::size_t>               jobs_failed_{0};
    std::atomic<std::size_t>               jobs_duplicates_{0};

    // Set true by dispatch()'s Err path when transfer-back-to-source
    // fails N times in a row. The heartbeat timer checks this and
    // becomes a no-op; the run() loop exits at the next iteration
    // boundary. Supervisor (systemd / orchestrator) is expected to
    // restart the process; in the meantime the orphan reaper (running
    // in the scheduler, a separate process) takes over the proc list.
    std::atomic<bool>                      fatal_failure_{false};
};

} // namespace wikore::ingest