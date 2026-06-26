#include "mohio/db.hpp"
#include <drogon/drogon.h>
#include <drogon/orm/DbConfig.h>
#include <spdlog/spdlog.h>
#include <regex>
#include <stdexcept>

namespace mohio {

namespace {

struct PgParams {
    std::string host     = "localhost";
    std::string user;
    std::string password;
    std::string dbname;
    int         port     = 5432;
};

// Parses postgresql://user:pass@host:port/dbname.
// Throws if the URL doesn't match.
PgParams parse_pg_url(const std::string& url) {
    static const std::regex re(
        R"(postgresql(?:sql)?://([^:@/]*)(?::([^@]*))?@([^:/]+)(?::(\d+))?/(.+))");
    std::smatch m;
    if (!std::regex_match(url, m, re))
        throw std::runtime_error("Cannot parse DATABASE_URL (expected "
                                 "postgresql://user:pass@host:port/db): " + url);
    PgParams p;
    p.user     = m[1].str();
    p.password = m[2].str();
    p.host     = m[3].str();
    if (m[4].length()) p.port = std::stoi(m[4].str());
    p.dbname   = m[5].str();
    return p;
}

} // namespace

void Db::init(const Config& cfg, int pool_size) {
    auto p = parse_pg_url(cfg.database_url);
    spdlog::info("[db] connect: {}@{}:{}/{} pool={}",
                 p.user, p.host, p.port, p.dbname, pool_size);
    drogon::app().addDbClient(drogon::orm::PostgresConfig{
        .host             = p.host,
        .port             = static_cast<unsigned short>(p.port),
        .databaseName     = p.dbname,
        .username         = p.user,
        .password         = p.password,
        .connectionNumber = static_cast<size_t>(pool_size),
        .name             = "default",
        .isFast           = false,
        .characterSet     = "",
        .timeout          = 30.0,
        .autoBatch        = false,
    });
}

drogon::orm::DbClientPtr Db::get() {
    return drogon::app().getDbClient();
}

} // namespace mohio
