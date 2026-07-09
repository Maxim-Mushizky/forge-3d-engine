#pragma once

#include <json.hpp> // nlohmann, bundled with tinygltf

#include <functional>
#include <string>
#include <vector>

namespace forge {

// Result of one tool invocation: MCP content blocks + execution-error flag.
// isError=true is for runtime failures the agent should read and self-correct;
// protocol-level problems (unknown tool, bad request) become JSON-RPC errors.
struct ToolResult {
    nlohmann::json content = nlohmann::json::array(); // MCP content blocks
    bool isError = false;

    static ToolResult Text(std::string text, bool error = false);
};

// Dotted path ("distance", "position[0]", "params.depth") of the first number
// inside `args` that is not float-finite; empty when the value is clean. NaN
// and Inf pass is_number() and every range check (fabs(NaN) < eps is false),
// so both tool dispatch and the script bindings refuse arguments up front
// instead of committing garbage into transforms and meshes (#104).
std::string NonFiniteArgPath(const nlohmann::json& args);

// First top-level key of `args` that is not in `validKeys`; empty when every
// key is accepted. Handlers read fields opportunistically (args.contains), so
// a typo'd or renamed key was silently dropped — set_transform{translation=...}
// "succeeded" while moving nothing. Rejecting up front turns that into a hard
// script error instead (#170). Only top-level keys: nested sub-schemas
// (spawn params.*, set_expression weights.*) stay the handler's business.
std::string UnknownArgKey(const nlohmann::json& args, const std::vector<const char*>& validKeys);

using ToolHandler = std::function<ToolResult(const nlohmann::json& args)>;
// Async tools receive a responder they may store and call on a later frame
// (e.g. an amortized path-traced render). Call it exactly once; extra calls
// are ignored.
using ToolResponder = std::function<void(ToolResult)>;
using AsyncToolHandler = std::function<void(const nlohmann::json& args, ToolResponder respond)>;
using ResourceReader = std::function<std::string()>; // returns text content

// GL-free MCP protocol kernel: JSON-RPC 2.0 framing + dispatch of the MCP
// methods (initialize, ping, tools/*, resources/*) over registered handlers.
// Owns no sockets or threads — McpServer marshals messages in from HTTP (#75).
class McpProtocol {
public:
    void RegisterTool(std::string name, std::string description,
                      nlohmann::json inputSchema, ToolHandler handler);
    void RegisterToolAsync(std::string name, std::string description,
                           nlohmann::json inputSchema, AsyncToolHandler handler);
    void RegisterResource(std::string uri, std::string name, std::string description,
                          std::string mimeType, ResourceReader reader);

    // Handles one raw JSON-RPC message. `respond` fires exactly once — inline
    // for everything except async tools, which may answer frames later. An
    // empty string means no response body is owed (notification -> HTTP 202).
    void HandleMessage(const std::string& body, std::function<void(std::string)> respond);

private:
    struct Tool {
        std::string name;
        std::string description;
        nlohmann::json inputSchema;
        AsyncToolHandler handler; // sync tools are wrapped at registration
    };
    struct Resource {
        std::string uri;
        std::string name;
        std::string description;
        std::string mimeType;
        ResourceReader reader;
    };

    std::vector<Tool> m_Tools;         // registration order = tools/list order
    std::vector<Resource> m_Resources;
};

} // namespace forge
