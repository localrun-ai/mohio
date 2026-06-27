#include "wikore/config.hpp"
#include "wikore/db.hpp"
#include <spdlog/spdlog.h>
#include <drogon/drogon.h>
#include <cstdlib>

// wikore-eval: evaluation harness CLI
// Replays golden questions against a running wikore-api instance and
// records results to eval_runs / eval_grades tables.
// Run on demand (not every PR) because it requires a live LLM endpoint.
//
// Usage: EVAL_TARGET_URL=http://localhost:9000 wikore-eval --corpus tests/fixtures/corpus/
//
// Iteration 0: startup stub only. Harness implemented in Iteration 5.

int main(int argc, char** /*argv*/) {
    const auto cfg = wikore::Config::from_env();

    spdlog::info("[wikore-eval] version: " WIKORE_GIT_HASH);

    if (argc < 2) {
        spdlog::error("[wikore-eval] usage: wikore-eval --corpus <path>");
        return EXIT_FAILURE;
    }

    spdlog::warn("[wikore-eval] eval harness not yet implemented (Iteration 5)");
    return EXIT_SUCCESS;
}
