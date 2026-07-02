#include "McpProtocol.h"

#include <exception>
#include <utility>

namespace forge {

using nlohmann::json;

namespace {

// JSON-RPC 2.0 error codes + MCP's resource-not-found extension.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kResourceNotFound = -32002;

json MakeError(const json& id, int code, std::string message, json data = {})
{
    json err{{"code", code}, {"message", std::move(message)}};
    if (!data.is_null())
        err["data"] = std::move(data);
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(err)}};
}

json MakeResult(const json& id, json result)
{
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

} // namespace

ToolResult ToolResult::Text(std::string text, bool error)
{
    ToolResult r;
    r.content = json::array({{{"type", "text"}, {"text", std::move(text)}}});
    r.isError = error;
    return r;
}

void McpProtocol::RegisterTool(std::string name, std::string description,
                               json inputSchema, ToolHandler handler)
{
    m_Tools.push_back({std::move(name), std::move(description), std::move(inputSchema),
                       std::move(handler)});
}

void McpProtocol::RegisterResource(std::string uri, std::string name, std::string description,
                                   std::string mimeType, ResourceReader reader)
{
    m_Resources.push_back({std::move(uri), std::move(name), std::move(description),
                           std::move(mimeType), std::move(reader)});
}

std::string McpProtocol::HandleMessage(const std::string& body)
{
    json msg = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded())
        return MakeError(nullptr, kParseError, "Parse error").dump();

    // The MCP spec (2025-06-18+) removed JSON-RPC batching: every message is a
    // single object. Anything else — including arrays — is an invalid request.
    if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg["method"].is_string())
        return MakeError(msg.is_object() ? msg.value("id", json()) : json(), kInvalidRequest,
                         "Invalid Request")
            .dump();

    const std::string method = msg["method"];
    const json params = msg.value("params", json::object());

    // No id = notification: act if we care (we don't, yet), never respond.
    if (!msg.contains("id"))
        return {};
    const json id = msg["id"];

    if (method == "initialize") {
        // Single-version server: always answer with our revision; per spec the
        // client decides whether to proceed or disconnect.
        return MakeResult(id, json{{"protocolVersion", "2025-11-25"},
                                   {"capabilities",
                                    {{"tools", json::object()}, {"resources", json::object()}}},
                                   {"serverInfo",
                                    {{"name", "forge"},
                                     {"title", "Forge Editor"},
                                     {"version", "0.1.0"}}}})
            .dump();
    }

    if (method == "ping")
        return MakeResult(id, json::object()).dump();

    if (method == "tools/list") {
        json tools = json::array();
        for (const Tool& t : m_Tools)
            tools.push_back({{"name", t.name},
                             {"description", t.description},
                             {"inputSchema", t.inputSchema}});
        return MakeResult(id, json{{"tools", std::move(tools)}}).dump();
    }

    if (method == "tools/call") {
        // contains() first: const operator[] on a missing key is UB in nlohmann.
        if (!params.contains("name") || !params["name"].is_string())
            return MakeError(id, kInvalidParams, "Missing tool name").dump();
        const std::string name = params["name"];
        for (const Tool& t : m_Tools) {
            if (t.name != name)
                continue;
            // Execution failures become isError results so the agent can read
            // the message and retry; only protocol misuse gets JSON-RPC errors.
            ToolResult r;
            try {
                r = t.handler(params.value("arguments", json::object()));
            } catch (const std::exception& e) {
                r = ToolResult::Text(e.what(), /*error=*/true);
            }
            return MakeResult(id, json{{"content", std::move(r.content)},
                                       {"isError", r.isError}})
                .dump();
        }
        return MakeError(id, kInvalidParams, "Unknown tool: " + name).dump();
    }

    if (method == "resources/list") {
        json resources = json::array();
        for (const Resource& r : m_Resources)
            resources.push_back({{"uri", r.uri},
                                 {"name", r.name},
                                 {"description", r.description},
                                 {"mimeType", r.mimeType}});
        return MakeResult(id, json{{"resources", std::move(resources)}}).dump();
    }

    if (method == "resources/read") {
        const std::string uri = params.value("uri", "");
        for (const Resource& r : m_Resources) {
            if (r.uri != uri)
                continue;
            json contents = json::array(
                {{{"uri", r.uri}, {"mimeType", r.mimeType}, {"text", r.reader()}}});
            return MakeResult(id, json{{"contents", std::move(contents)}}).dump();
        }
        return MakeError(id, kResourceNotFound, "Resource not found", json{{"uri", uri}}).dump();
    }

    return MakeError(id, kMethodNotFound, "Method not found: " + method).dump();
}

} // namespace forge
