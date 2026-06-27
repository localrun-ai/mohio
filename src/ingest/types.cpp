#include "wikore/ingest/types.hpp"

namespace wikore::ingest {

std::string_view to_string(IngestStatus s)
{
    switch (s) {
    case IngestStatus::pending: return "pending";
    case IngestStatus::running: return "running";
    case IngestStatus::done:    return "done";
    case IngestStatus::failed:  return "failed";
    }
    return "unknown";
}

std::optional<IngestStatus> ingest_status_from_string(std::string_view s)
{
    if (s == "pending") return IngestStatus::pending;
    if (s == "running") return IngestStatus::running;
    if (s == "done")    return IngestStatus::done;
    if (s == "failed")  return IngestStatus::failed;
    return std::nullopt;
}

} // namespace wikore::ingest
