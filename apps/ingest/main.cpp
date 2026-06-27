#include "wikore/config.hpp"
#include "wikore/db.hpp"
#include "wikore/redis.hpp"
#include <spdlog/spdlog.h>
#include <drogon/drogon.h>

// wikore-ingest: document ingestion worker
// Consumes lr:ingest:q:{company_id} Redis lists (per-tenant fair scheduling).
// Parses documents, chunks, embeds, and pushes to Qdrant.
// Claims outbox_events of type qdrant_upsert_chunk_payload after each chunk batch.
// Iteration 0: startup stub only. Ingest pipeline implemented in Iteration 1.

int main() {
    const auto cfg = wikore::Config::from_env();

    spdlog::info("[wikore-ingest] version: " WIKORE_GIT_HASH);
    spdlog::info("[wikore-ingest] db:      {}", cfg.database_url);
    spdlog::info("[wikore-ingest] redis:   {}", cfg.redis_url);
    spdlog::info("[wikore-ingest] qdrant:  {}", cfg.qdrant_url);
    spdlog::info("[wikore-ingest] embed:   {}", cfg.embed_base_url);

    // Drogon is used for its async PG client even in workers.
    wikore::Db::init(cfg, /*pool_size=*/4);
    wikore::Redis::init(cfg);

    spdlog::warn("[wikore-ingest] ingest pipeline not yet implemented (Iteration 1)");

    // Start Drogon event loop (required for async PG/Redis clients).
    // No HTTP listener registered; this is a pure worker process.
    drogon::app()
        .setThreadNum(2)
        .run();
}
