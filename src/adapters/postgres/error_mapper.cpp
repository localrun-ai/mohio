#include "wikore/adapters/postgres/error_mapper.hpp"
#include <drogon/orm/Exception.h>
#include <unordered_map>
#include <string>
#include <string_view>
#include <regex>

namespace wikore::postgres {

namespace {

constexpr std::string_view SQLSTATE_SERIALIZATION_FAILURE  = "40001";
constexpr std::string_view SQLSTATE_DEADLOCK               = "40P01";
constexpr std::string_view SQLSTATE_FK_VIOLATION           = "23503";
constexpr std::string_view SQLSTATE_NOT_NULL_VIOLATION     = "23502";

// Maps constraint name -> typed domain Error.
// Every named constraint from V001-V015 must appear here.
const std::unordered_map<std::string, Error> k_constraint_map = {
    // V003: document version lifecycle
    {"document_versions_one_active_per_doc_uidx",
        Error::conflict("document already has an active version")},
    {"document_versions_active_state_chk",
        Error::invalid_state("active version must have completed ingest and activated_at")},
    {"document_versions_done_state_chk",
        Error::invalid_state("done ingest must have completed_at and chunk_count")},
    {"document_versions_active_requires_done_chk",
        Error::invalid_state("active lifecycle requires completed ingest")},
    {"document_versions_superseded_after_activated_chk",
        Error::invalid_state("superseded_at must be after activated_at")},
    {"document_versions_deprecated_interval_chk",
        Error::invalid_state("deprecated version with activated_at must have superseded_at")},

    // V001: org unit constraints
    {"org_unit_self_loop_chk",
        Error::invalid_input("org unit cannot be its own parent")},
    {"org_units_one_root_per_company_uidx",
        Error::conflict("company already has a root org unit")},

    // V002: membership and grant constraints
    {"memberships_user_org_uidx",
        Error::conflict("membership already exists for this user and org unit")},
    {"memberships_group_org_uidx",
        Error::conflict("membership already exists for this group and org unit")},
    {"memberships_expires_after_granted_chk",
        Error::invalid_input("membership expires_at must be after granted_at")},
    {"resource_grants_target_chk",
        Error::invalid_input("grant must target either a user or a group, not both")},

    // V005: chat turn JSONB array constraints
    {"chat_turns_rag_sources_array_chk",
        Error::invalid_state("rag_sources must be a JSON array")},
    {"chat_turns_tool_calls_array_chk",
        Error::invalid_state("tool_calls must be a JSON array")},

    // V014: sensitivity label values
    {"document_versions_sensitivity_label_check",
        Error::invalid_input("sensitivity_label must be public, internal, confidential, or restricted")},
    {"documents_sensitivity_label_default_check",
        Error::invalid_input("sensitivity_label_default must be public, internal, confidential, or restricted")},

    // V015: outbox idempotency
    {"outbox_events_company_id_job_type_idempotency_key_key",
        Error::conflict("outbox event with this idempotency key already exists")},
};

} // namespace

namespace {

// Drogon's Postgres driver does not expose PG_DIAG_CONSTRAINT_NAME directly.
// Extract the constraint name from the PG error message text, which contains
// the pattern: constraint "constraint_name"
std::string extract_constraint(const std::string& msg) {
    static const std::regex re(R"re(constraint "([^"]+)")re");
    std::smatch m;
    if (std::regex_search(msg, m, re))
        return m[1].str();
    return {};
}

} // namespace

Error map_db_exception(const drogon::orm::DrogonDbException& ex) {
    // DrogonDbException is not std::exception; use .base() then dynamic_cast
    // to SqlError to access SQLSTATE.
    const auto* sql_err = dynamic_cast<const drogon::orm::SqlError*>(&ex.base());

    const std::string msg        = ex.base().what();
    const std::string sqlstate   = sql_err ? sql_err->sqlState() : "";
    const std::string constraint = extract_constraint(msg);

    if (sqlstate == SQLSTATE_SERIALIZATION_FAILURE || sqlstate == SQLSTATE_DEADLOCK)
        return Error::conflict("transaction conflict, retry: " + msg);

    if (!constraint.empty()) {
        auto it = k_constraint_map.find(constraint);
        if (it != k_constraint_map.end())
            return it->second;
        return Error::database_error("unmapped constraint [" + constraint + "]: " + msg);
    }

    if (sqlstate == SQLSTATE_FK_VIOLATION)
        return Error::invalid_input("referenced entity does not exist or belongs to a different company");

    if (sqlstate == SQLSTATE_NOT_NULL_VIOLATION)
        return Error::invalid_input("required field is missing: " + msg);

    return Error::database_error("database error [" + sqlstate + "]: " + msg);
}

} // namespace wikore::postgres
