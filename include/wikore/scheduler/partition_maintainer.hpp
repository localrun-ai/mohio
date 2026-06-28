#pragma once
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <atomic>
#include <chrono>
#include <functional>

namespace wikore::scheduler {

// ---------------------------------------------------------------------------
// PartitionMaintainer - ensures time-partitioned tables never run out of
// partitions and alerts if rows fall into the catch-all DEFAULT partition.
//
// Two tables are maintained:
//   audit_log    (V007) - quarterly partitions, named audit_log_YYYY_qN
//   usage_events (V016) - monthly partitions, named usage_events_YYYY_MM
//
// Both tables have a *_default catch-all that prevents INSERT failures, but
// rows landing there cannot be moved to named partitions without a locking
// maintenance op. This job runs ahead of the frontier and alerts immediately
// if any overflow is detected.
//
// Runs once at startup and then every `interval` (default 24h). A daily
// run with a lookahead of 4 quarters / 12 months keeps at least a year of
// partitions pre-created at all times.
// ---------------------------------------------------------------------------

class PartitionMaintainer {
public:
    using ShutdownPredicate = std::function<bool()>;

    struct Options {
        std::chrono::hours interval               = std::chrono::hours(24);
        int                audit_log_lookahead    = 4;   // quarters
        int                usage_events_lookahead = 12;  // months
    };

    PartitionMaintainer(drogon::orm::DbClientPtr db,
                        ShutdownPredicate        shutdown_requested,
                        Options                  opts);

    drogon::Task<void> run();

    // Single sweep: creates missing partitions and checks for overflow.
    // Exposed for testing.
    drogon::Task<void> run_once();

    std::size_t partitions_created() const { return partitions_created_.load(); }

private:
    drogon::Task<void> ensure_audit_log_partitions();
    drogon::Task<void> ensure_usage_events_partitions();
    drogon::Task<void> check_default_overflow();

    drogon::orm::DbClientPtr db_;
    ShutdownPredicate        shutdown_;
    Options                  opts_;
    std::atomic<std::size_t> partitions_created_{0};
};

} // namespace wikore::scheduler
