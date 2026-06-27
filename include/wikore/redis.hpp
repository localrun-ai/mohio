#pragma once
#include <string>
#include <optional>
#include <chrono>
#include "wikore/config.hpp"

namespace wikore {

// Thread-safe synchronous Redis client backed by a fixed connection pool.
// Silently degrades (nullopt / no-op) when Redis is unavailable or not
// configured. All operations use a 2-second socket timeout.
//
// Design note: synchronous hiredis is deliberate. Auth cache hits take ~0.1ms
// on localhost - negligible against the LLM latency budget. Async hiredis
// requires non-trivial Drogon event-loop bridging with no measurable benefit
// for this access pattern.
struct Redis {
    // Parse REDIS_URL and create a pool of `pool_size` connections.
    // No-op (warns) if cfg.redis_url is empty.
    static void init(const Config& cfg, int pool_size = 8);

    static std::optional<std::string> get(std::string_view key);

    // ttl == 0 means no expiry (plain SET).
    static void set(std::string_view key, std::string_view value,
                    std::chrono::seconds ttl = {});

    static void del(std::string_view key);
};

} // namespace wikore
