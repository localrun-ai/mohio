#pragma once
#include <drogon/orm/DbClient.h>
#include "wikore/config.hpp"

namespace wikore {

// Thin accessor for Drogon's built-in async PostgreSQL connection pool.
// Call Db::init() once in main() before drogon::app().run().
// Everywhere else, call Db::get() to obtain the shared client.
struct Db {
    static void init(const Config& cfg, int pool_size = 8);
    static drogon::orm::DbClientPtr get();
};

} // namespace wikore
