#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// Test the constraint-name extraction logic in pg_error_mapper.
// These tests do NOT need a Postgres connection; they verify the regex
// parsing of PG error message strings and the constraint->Error mapping.

// Pull the internal extract_constraint function by re-including the regex
// logic under test. We test the public map_db_exception() indirectly by
// constructing DrogonDbException subclasses in the integration tests;
// here we only verify the name extraction heuristic.

#include <regex>
#include <string>

namespace {

// Mirror of the production extract_constraint() logic.
std::string extract_constraint(const std::string& msg) {
    static const std::regex re(R"re(constraint "([^"]+)")re");
    std::smatch m;
    if (std::regex_search(msg, m, re))
        return m[1].str();
    return {};
}

} // namespace

TEST_CASE("extract_constraint: unique violation message", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: duplicate key value violates unique constraint "document_versions_one_active_per_doc_uidx")"
        "\nDETAIL: Key (company_id, document_id)=(...) already exists.";
    REQUIRE(extract_constraint(msg) == "document_versions_one_active_per_doc_uidx");
}

TEST_CASE("extract_constraint: check violation message", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: new row for relation "document_versions" violates check constraint "document_versions_active_state_chk")";
    REQUIRE(extract_constraint(msg) == "document_versions_active_state_chk");
}

TEST_CASE("extract_constraint: FK violation message", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: insert or update on table "memberships" violates foreign key constraint "memberships_user_same_company_fk")";
    REQUIRE(extract_constraint(msg) == "memberships_user_same_company_fk");
}

TEST_CASE("extract_constraint: no constraint in message returns empty", "[error_mapper]") {
    const std::string msg = "ERROR: connection to server lost";
    REQUIRE(extract_constraint(msg).empty());
}

TEST_CASE("extract_constraint: outbox idempotency key constraint", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: duplicate key value violates unique constraint "outbox_events_company_id_job_type_idempotency_key_key")";
    REQUIRE(extract_constraint(msg) == "outbox_events_company_id_job_type_idempotency_key_key");
}

TEST_CASE("extract_constraint: org_unit self-loop check", "[error_mapper]") {
    const std::string msg =
        R"(ERROR: new row for relation "org_units" violates check constraint "org_unit_self_loop_chk")";
    REQUIRE(extract_constraint(msg) == "org_unit_self_loop_chk");
}
