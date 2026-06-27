#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <uuid/uuid.h>  // libuuid for UUID generation

namespace wikore {

// ---------------------------------------------------------------------------
// UUID helpers
// ---------------------------------------------------------------------------

using Uuid = std::string;   // canonical lower-case hyphenated string

inline Uuid uuid_generate() {
    uuid_t raw;
    uuid_generate_random(raw);
    char buf[37];
    uuid_unparse_lower(raw, buf);
    return Uuid{buf};
}

// ---------------------------------------------------------------------------
// Error type
//
// Business failures are returned as Error values (not exceptions).
// Exceptions are reserved for programmer bugs (invariant violations, OOM).
// ---------------------------------------------------------------------------

struct Error {
    enum class Kind {
        NotFound,
        Conflict,
        Forbidden,
        InvalidInput,
        InvalidState,
        DatabaseError,
        ServiceUnavailable,
    };

    Kind        kind;
    std::string message;

    static Error not_found(std::string msg)          { return {Kind::NotFound,           std::move(msg)}; }
    static Error conflict(std::string msg)            { return {Kind::Conflict,            std::move(msg)}; }
    static Error forbidden(std::string msg)           { return {Kind::Forbidden,           std::move(msg)}; }
    static Error invalid_input(std::string msg)       { return {Kind::InvalidInput,        std::move(msg)}; }
    static Error invalid_state(std::string msg)       { return {Kind::InvalidState,        std::move(msg)}; }
    static Error database_error(std::string msg)      { return {Kind::DatabaseError,       std::move(msg)}; }
    static Error unavailable(std::string msg)         { return {Kind::ServiceUnavailable,  std::move(msg)}; }
};

template<class T>
using Result = std::expected<T, Error>;

// ---------------------------------------------------------------------------
// Tenant
// ---------------------------------------------------------------------------

struct Tenant {
    Uuid company_id;
};

// ---------------------------------------------------------------------------
// Principal
//
// Represents the authenticated caller. auth layer populates this from
// the validated JWT or API key. deactivated_at is NOT here: auth middleware
// must reject deactivated users before RequestContext is built.
// ---------------------------------------------------------------------------

struct Principal {
    Uuid        user_id;
    std::string email;
    std::string display_name;
    bool        is_admin = false;
    bool        is_service_account = false; // true for API-key callers
};

// ---------------------------------------------------------------------------
// AccessScope
//
// Resolved lazily via AccessResolver port. NOT stored in RequestContext
// directly; caller requests it when needed.
// ---------------------------------------------------------------------------

struct AccessScope {
    std::vector<Uuid>                          org_unit_ids;
    std::chrono::system_clock::time_point      cache_until;
    int                                        access_epoch = 0;
};

// ---------------------------------------------------------------------------
// TraceSpan
//
// Opaque carrier for distributed tracing. The no-op Tracer port produces
// empty spans. The OTel SDK adapter produces real spans.
// ---------------------------------------------------------------------------

struct TraceSpan {
    std::string trace_id;  // UUID, per-request, propagated to all logs
    std::string span_id;   // UUID, per-span
    // Additional vendor-specific fields live in the Tracer adapter.
};

// ---------------------------------------------------------------------------
// RequestContext
//
// Immutable per-request bundle. Passed by const& to every use case and
// repository method. Never stored, never placed in thread-local storage.
// ---------------------------------------------------------------------------

struct RequestContext {
    Tenant                                  tenant;
    Principal                               principal;
    TraceSpan                               span;
    std::chrono::steady_clock::time_point   deadline;

    bool deadline_exceeded() const {
        return std::chrono::steady_clock::now() >= deadline;
    }
};

} // namespace wikore
