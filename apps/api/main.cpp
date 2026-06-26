#include "mohio/config.hpp"
#include "mohio/auth.hpp"
#include "mohio/db.hpp"
#include "mohio/redis.hpp"
#include "mohio/org_tree.hpp"
#include "mohio/access.hpp"

#include <drogon/drogon.h>
#include <spdlog/spdlog.h>
#include <format>

// ---------------------------------------------------------------------------
// Route stubs - each will move to its own handler file as implemented.
// All handlers are coroutines; filters applied per-group.
// ---------------------------------------------------------------------------

using Req = drogon::HttpRequestPtr;
using CB  = std::function<void(const drogon::HttpResponsePtr&)>;

static drogon::HttpResponsePtr json_ok(std::string body) {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(drogon::k200OK);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->setBody(std::move(body));
    return r;
}

static drogon::HttpResponsePtr not_implemented() {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(drogon::k501NotImplemented);
    r->setBody(R"({"error":"not implemented"})");
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    return r;
}

int main() {
    const auto cfg = mohio::Config::from_env();

    spdlog::info("[mohio] commit:  {}", MOHIO_GIT_HASH);
    spdlog::info("[mohio] port:    {}", cfg.port);
    spdlog::info("[mohio] db:      {}", cfg.database_url);
    spdlog::info("[mohio] qdrant:  {}", cfg.qdrant_url);
    spdlog::info("[mohio] llm:     {}", cfg.llm_base_url);

    // Register Drogon DB client before run() so getDbClient() works in filters.
    mohio::Db::init(cfg);
    mohio::Redis::init(cfg);
    // Fetch OIDC JWKS synchronously - Drogon event loop not yet started here,
    // so we use a direct SSL BIO call inside auth_init.
    mohio::auth_init(cfg);

    // -----------------------------------------------------------------------
    // Health (public)
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/health",
        [](const Req&, CB&& cb) {
            cb(json_ok(R"({"ok":true})"));
        }, {drogon::Get});

    // -----------------------------------------------------------------------
    // Identity
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/me",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Get}, {"mohio::AuthFilter"});

    // -----------------------------------------------------------------------
    // Org tree
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/tree",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Get}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::HttpMethod::Patch, drogon::Delete},
        {"mohio::AuthFilter"});

    // Members
    drogon::app().registerHandler("/api/orgs/{1}/members",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/members/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::HttpMethod::Patch, drogon::Delete}, {"mohio::AuthFilter"});

    // Cross-org grants
    drogon::app().registerHandler("/api/orgs/{1}/grants",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/grants/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Delete}, {"mohio::AuthFilter"});

    // -----------------------------------------------------------------------
    // Documents / RAG ingest
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/docs",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/docs/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Delete}, {"mohio::AuthFilter"});

    // -----------------------------------------------------------------------
    // Chat (SSE)
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/chat/stream",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/chat/sessions",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/chat/sessions/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Delete}, {"mohio::AuthFilter"});

    // -----------------------------------------------------------------------
    // Wiki
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/wiki/pages",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/pages/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Delete}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/ingest",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/query",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/wiki/lint",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Post}, {"mohio::AuthFilter"});

    // -----------------------------------------------------------------------
    // Integrations / MCP
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/orgs/{1}/integrations",
        [](const Req&, CB&& cb, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::Post}, {"mohio::AuthFilter"});

    drogon::app().registerHandler("/api/orgs/{1}/integrations/{2}",
        [](const Req&, CB&& cb, std::string, std::string) { cb(not_implemented()); },
        {drogon::Get, drogon::HttpMethod::Patch, drogon::Delete},
        {"mohio::AuthFilter"});

    // -----------------------------------------------------------------------
    // Admin
    // -----------------------------------------------------------------------
    drogon::app().registerHandler("/api/admin/audit",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Get}, {"mohio::AuthFilter", "mohio::AdminFilter"});

    drogon::app().registerHandler("/api/admin/users",
        [](const Req&, CB&& cb) { cb(not_implemented()); },
        {drogon::Get, drogon::Post},
        {"mohio::AuthFilter", "mohio::AdminFilter"});

    // -----------------------------------------------------------------------
    // Server config
    // -----------------------------------------------------------------------
    drogon::app()
        .setLogPath("./")
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("0.0.0.0", cfg.port)
        .setThreadNum(static_cast<int>(std::thread::hardware_concurrency()))
        .setIdleConnectionTimeout(120)
        .run();
}
