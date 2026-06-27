#include "wikore/ingest/types.hpp"

namespace wikore::ingest {

std::string_view to_string(IngestStatus s)
{
    switch (s) {
    case IngestStatus::pending:    return "pending";
    case IngestStatus::processing: return "processing";
    case IngestStatus::done:       return "done";
    case IngestStatus::error:      return "error";
    }
    return "unknown";
}

std::optional<IngestStatus> ingest_status_from_string(std::string_view s)
{
    if (s == "pending")    return IngestStatus::pending;
    if (s == "processing") return IngestStatus::processing;
    if (s == "done")       return IngestStatus::done;
    if (s == "error")      return IngestStatus::error;
    return std::nullopt;
}

} // namespace wikore::ingest
