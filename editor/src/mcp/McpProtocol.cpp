#include "McpProtocol.h"

#include <exception>
#include <memory>
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
    RegisterToolAsync(std::move(name), std::move(description), std::move(inputSchema),
                      [handler = std::move(handler)](const json& args, ToolResponder respond) {
                          respond(handler(args));
                      });
}

void McpProtocol::RegisterToolAsync(std::string name, std::string description,
                                    json inputSchema, AsyncToolHandler handler)
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

void McpProtocol::HandleMessage(const std::string& body,
                                std::function<void(std::string)> respond)
{
    json msg = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded()) {
        respond(MakeError(nullptr, kParseError, "Parse error").dump());
        return;
    }

    // The MCP spec (2025-06-18+) removed JSON-RPC batching: every message is a
    // single object. Anything else — including arrays — is an invalid request.
    // Compare the raw json value (never .value<string>()): present-but-wrong-
    // type keys would throw type_error out of the kernel.
    if (!msg.is_object() || msg["jsonrpc"] != "2.0" || !msg["method"].is_string()) {
        respond(MakeError(msg.is_object() ? msg.value("id", json()) : json(), kInvalidRequest,
                          "Invalid Request")
                    .dump());
        return;
    }

    const std::string method = msg["method"];
    const json params = msg.value("params", json::object());

    // No id = notification: act if we care (we don't, yet), never respond
    // with a body — the HTTP layer answers 202.
    if (!msg.contains("id")) {
        respond({});
        return;
    }
    const json id = msg["id"];

    if (method == "initialize") {
        // Single-version server: always answer with our revision; per spec the
        // client decides whether to proceed or disconnect.
        respond(MakeResult(id, json{{"protocolVersion", "2025-11-25"},
                                    {"capabilities",
                                     {{"tools", json::object()}, {"resources", json::object()}}},
                                    {"serverInfo",
                                     {{"name", "forge"},
                                      {"title", "Forge Editor"},
                                      {"version", "0.1.0"}}}})
                    .dump());
        return;
    }

    if (method == "ping") {
        respond(MakeResult(id, json::object()).dump());
        return;
    }

    if (method == "tools/list") {
        json tools = json::array();
        for (const Tool& t : m_Tools)
            tools.push_back({{"name", t.name},
                             {"description", t.description},
                             {"inputSchema", t.inputSchema}});
        respond(MakeResult(id, json{{"tools", std::move(tools)}}).dump());
        return;
    }

    if (method == "tools/call") {
        // contains() first: const operator[] on a missing key is UB in nlohmann.
        if (!params.contains("name") || !params["name"].is_string()) {
            respond(MakeError(id, kInvalidParams, "Missing tool name").dump());
            return;
        }
        const std::string name = params["name"];
        for (const Tool& t : m_Tools) {
            if (t.name != name)
                continue;
            // The responder is idempotent (first call wins) so a handler bug
            // can't answer one HTTP request twice, and owns the id + respond
            // so async tools can fire it frames later.
            auto responded = std::make_shared<bool>(false);
            ToolResponder responder = [id, respond, responded](ToolResult r) {
                if (*responded)
                    return;
                *responded = true;
                respond(MakeResult(id, json{{"content", std::move(r.content)},
                                            {"isError", r.isError}})
                            .dump());
            };
            // Execution failures become isError results so the agent can read
            // the message and retry; only protocol misuse gets JSON-RPC errors.
            try {
                t.handler(params.value("arguments", json::object()), responder);
            } catch (const std::exception& e) {
                responder(ToolResult::Text(e.what(), /*error=*/true));
            }
            return;
        }
        respond(MakeError(id, kInvalidParams, "Unknown tool: " + name).dump());
        return;
    }

    if (method == "resources/list") {
        json resources = json::array();
        for (const Resource& r : m_Resources)
            resources.push_back({{"uri", r.uri},
                                 {"name", r.name},
                                 {"description", r.description},
                                 {"mimeType", r.mimeType}});
        respond(MakeResult(id, json{{"resources", std::move(resources)}}).dump());
        return;
    }

    if (method == "resources/read") {
        if (!params.contains("uri") || !params["uri"].is_string()) {
            respond(MakeError(id, kInvalidParams, "Missing resource uri").dump());
            return;
        }
        const std::string uri = params["uri"];
        for (const Resource& r : m_Resources) {
            if (r.uri != uri)
                continue;
            json contents = json::array(
                {{{"uri", r.uri}, {"mimeType", r.mimeType}, {"text", r.reader()}}});
            respond(MakeResult(id, json{{"contents", std::move(contents)}}).dump());
            return;
        }
        respond(MakeError(id, kResourceNotFound, "Resource not found", json{{"uri", uri}}).dump());
        return;
    }

    respond(MakeError(id, kMethodNotFound, "Method not found: " + method).dump());
}

} // namespace forge
