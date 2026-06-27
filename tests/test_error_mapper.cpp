#include <catch2/catch_test_macros.hpp>
#include "wikore/adapters/postgres/error_mapper_internal.hpp"

// Test the constraint-name extraction logic in pg_error_mapper.
// Uses the production extract_constraint() via error_mapper_internal.hpp so
// any change to the regex is immediately caught here.

using wikore::postgres::detail::extract_constraint;

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
