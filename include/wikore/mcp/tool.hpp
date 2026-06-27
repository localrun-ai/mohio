#pragma once
#include <string>
#include <string_view>
#include <drogon/drogon.h>

namespace wikore::mcp {

// JSON Schema snippet for a tool parameter.
struct ParamSchema {
    std::string name;
    std::string type;           // "string", "integer", "boolean"
    std::string description;
    bool        required = true;
};

// Descriptor forwarded to the LLM as a tool definition.
struct ToolDef {
    std::string              name;
    std::string              description;
    std::vector<ParamSchema> params;
    bool                     read_only = true; // false = never cache result
};

// Base class for all MCP tools.
// Each integration (Slack, Jira, ...) registers one or more tools.
class Tool {
public:
    virtual ~Tool() = default;

    virtual const ToolDef& definition() const = 0;

    // Invoke the tool with JSON args, return JSON result string.
    // credentials: decrypted AES-256-GCM JSON from integrations.credentials.
    virtual drogon::Task<std::string>
    invoke(std::string_view args_json,
           std::string_view credentials) = 0;
};

} // namespace wikore::mcp
