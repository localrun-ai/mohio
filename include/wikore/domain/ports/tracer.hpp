#pragma once
#include "wikore/domain/types.hpp"
#include <string_view>
#include <functional>

namespace wikore {

// ---------------------------------------------------------------------------
// Tracer port
//
// Abstracts distributed tracing. The no-op adapter is the default for MVP.
// The OTel SDK adapter is a later drop-in replacement; no application code
// changes because the port contract is stable.
// ---------------------------------------------------------------------------

class Tracer {
public:
    virtual ~Tracer() = default;

    // Start a child span under the current request's trace context.
    // Returns a guard: span ends when the guard goes out of scope.
    struct SpanGuard {
        virtual ~SpanGuard() = default;
        virtual void set_attribute(std::string_view key, std::string_view value) = 0;
        virtual void record_error(std::string_view message) = 0;
    };

    virtual std::unique_ptr<SpanGuard>
    start_span(const RequestContext& ctx, std::string_view operation_name) = 0;

    // Create a root TraceSpan for a new request (called by the HTTP transport layer).
    virtual TraceSpan new_request_span() = 0;
};

// ---------------------------------------------------------------------------
// NoopTracer - default adapter for MVP
//
// All methods are no-ops. Zero overhead: the SpanGuard is a trivial object.
// ---------------------------------------------------------------------------

class NoopTracer : public Tracer {
    struct NoopSpan : SpanGuard {
        void set_attribute(std::string_view, std::string_view) override {}
        void record_error(std::string_view) override {}
    };
public:
    std::unique_ptr<SpanGuard>
    start_span(const RequestContext&, std::string_view) override {
        return std::make_unique<NoopSpan>();
    }

    TraceSpan new_request_span() override {
        return TraceSpan{
            .trace_id = uuid_generate(),
            .span_id  = uuid_generate(),
        };
    }
};

} // namespace wikore
