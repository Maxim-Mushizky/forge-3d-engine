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
