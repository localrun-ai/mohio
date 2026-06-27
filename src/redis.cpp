#include "wikore/redis.hpp"
#include <hiredis/hiredis.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace wikore {

namespace {

struct Slot {
    std::mutex    mu;
    redisContext* ctx = nullptr;
};

struct Pool {
    std::vector<std::unique_ptr<Slot>> slots;
    std::string host     = "127.0.0.1";
    std::string password;
    int         port     = 6379;
    int         db       = 0;
    std::atomic<size_t> rr{0};

    redisContext* connect_one() const {
        timeval tv{2, 0};
        redisContext* c = redisConnectWithTimeout(host.c_str(), port, tv);
        if (!c || c->err) {
            std::string e = c ? c->errstr : "null context";
            if (c) redisFree(c);
            throw std::runtime_error(std::string("[redis] connect failed: ") + e);
        }
        redisSetTimeout(c, tv);
        if (!password.empty()) {
            auto* r = static_cast<redisReply*>(
                redisCommand(c, "AUTH %s", password.c_str()));
            if (r) freeReplyObject(r);
        }
        if (db != 0) {
            auto* r = static_cast<redisReply*>(redisCommand(c, "SELECT %d", db));
            if (r) freeReplyObject(r);
        }
        return c;
    }
};

static std::unique_ptr<Pool> g_pool;

static void parse_redis_url(Pool& p, const std::string& url) {
    if (url.rfind("redis://", 0) != 0) {
        spdlog::warn("[redis] unexpected URL scheme, using defaults");
        return;
    }
    std::string rest = url.substr(8); // after "redis://"

    // Extract optional ":password@" or "user:password@"
    auto at = rest.rfind('@');
    if (at != std::string::npos) {
        auto colon = rest.find(':', 0);
        if (colon != std::string::npos && colon < at)
            p.password = rest.substr(colon + 1, at - colon - 1);
        rest = rest.substr(at + 1);
    }

    // host[:port][/db]
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    if (slash != std::string::npos && slash + 1 < rest.size())
        p.db = std::stoi(rest.substr(slash + 1));

    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        p.host = hostport.substr(0, colon);
        p.port = std::stoi(hostport.substr(colon + 1));
    } else {
        p.host = hostport;
    }
}

static Slot* pick_slot() {
    if (!g_pool || g_pool->slots.empty()) return nullptr;
    size_t idx = g_pool->rr.fetch_add(1, std::memory_order_relaxed)
                 % g_pool->slots.size();
    return g_pool->slots[idx].get();
}

// Reconnects automatically if the context died (network hiccup, server restart).
template<typename F>
static redisReply* slot_exec(Slot& slot, F&& make_reply) {
    std::lock_guard lk(slot.mu);
    if (!slot.ctx) {
        try { slot.ctx = g_pool->connect_one(); }
        catch (const std::exception& ex) {
            spdlog::warn("{}", ex.what());
            return nullptr;
        }
    }
    redisReply* r = make_reply(slot.ctx);
    if (!r || slot.ctx->err) {
        redisFree(slot.ctx);
        slot.ctx = nullptr;
        if (r) freeReplyObject(r);
        return nullptr;
    }
    return r;
}

} // namespace

void Redis::init(const Config& cfg, int pool_size) {
    if (cfg.redis_url.empty()) {
        spdlog::warn("[redis] REDIS_URL not set - caching disabled");
        return;
    }
    auto pool = std::make_unique<Pool>();
    parse_redis_url(*pool, cfg.redis_url);
    spdlog::info("[redis] {}:{}/{} pool={}", pool->host, pool->port, pool->db, pool_size);

    for (int i = 0; i < pool_size; ++i) {
        auto slot = std::make_unique<Slot>();
        try { slot->ctx = pool->connect_one(); }
        catch (const std::exception& ex) {
            spdlog::warn("[redis] slot {} lazy-connect: {}", i, ex.what());
        }
        pool->slots.push_back(std::move(slot));
    }
    g_pool = std::move(pool);
}

std::optional<std::string> Redis::get(std::string_view key) {
    auto* slot = pick_slot();
    if (!slot) return std::nullopt;

    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "GET %b", key.data(), key.size()));
    });
    if (!r) return std::nullopt;

    std::optional<std::string> result;
    if (r->type == REDIS_REPLY_STRING)
        result = std::string(r->str, static_cast<size_t>(r->len));
    freeReplyObject(r);
    return result;
}

void Redis::set(std::string_view key, std::string_view value,
                std::chrono::seconds ttl) {
    auto* slot = pick_slot();
    if (!slot) return;

    auto* r = slot_exec(*slot, [&](redisContext* c) -> redisReply* {
        if (ttl.count() > 0) {
            return static_cast<redisReply*>(
                redisCommand(c, "SETEX %b %lld %b",
                             key.data(), key.size(),
                             static_cast<long long>(ttl.count()),
                             value.data(), value.size()));
        }
        return static_cast<redisReply*>(
            redisCommand(c, "SET %b %b", key.data(), key.size(), value.data(), value.size()));
    });
    if (r) freeReplyObject(r);
}

void Redis::del(std::string_view key) {
    auto* slot = pick_slot();
    if (!slot) return;
    auto* r = slot_exec(*slot, [&](redisContext* c) {
        return static_cast<redisReply*>(
            redisCommand(c, "DEL %b", key.data(), key.size()));
    });
    if (r) freeReplyObject(r);
}

} // namespace wikore
