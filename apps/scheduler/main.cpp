#include "wikore/config.hpp"
#include "wikore/db.hpp"
#include "wikore/redis.hpp"
#include <spdlog/spdlog.h>
#include <drogon/drogon.h>

// wikore-scheduler: single-replica periodic worker
// Responsibilities:
//   - Outbox event consumer (qdrant resync, redis invalidation)
//   - Stuck-ingest recovery: re-enqueue document_versions with
//     ingest_status='pending' older than 15 minutes
//   - Tombstone GC: delete Qdrant points for documents with deleted_at set
//   - Review-due sweeps, cache warmup (Iteration 4+)
//
// Advisory lock pattern: pg_try_advisory_lock(hashtext('wikore-scheduler'))
// ensures only one scheduler instance runs sweeps at a time even under
// horizontal deployment.
//
// Iteration 0: startup stub only. Sweep loops implemented in Iteration 1+.

int main() {
    const auto cfg = wikore::Config::from_env();

    spdlog::info("[wikore-scheduler] version: " WIKORE_GIT_HASH);
    spdlog::info("[wikore-scheduler] db:      {}", cfg.database_url);
    spdlog::info("[wikore-scheduler] redis:   {}", cfg.redis_url);

    wikore::Db::init(cfg, /*pool_size=*/2);
    wikore::Redis::init(cfg);

    spdlog::warn("[wikore-scheduler] sweep loops not yet implemented (Iteration 1)");

    drogon::app()
        .setThreadNum(1)
        .run();
}
