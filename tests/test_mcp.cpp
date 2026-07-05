#include "test_framework.h"

#include "mcp/McpProtocol.h"

#include <json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

// Exercises the GL-free MCP protocol kernel: JSON-RPC 2.0 framing, lifecycle,
// tool/resource dispatch, and the error taxonomy (protocol errors vs isError
// tool results). No sockets, no threads — HandleMessage in, JSON out.

namespace forge::test {
namespace {

using nlohmann::json;

McpProtocol MakeProtocolWithPing()
{
    McpProtocol proto;
    proto.RegisterTool("ping", "Health check; returns pong.",
                       json{{"type", "object"}, {"additionalProperties", false}},
                       [](const json&) { return ToolResult::Text("pong"); });
    return proto;
}

std::string Request(const char* method, json params = json::object(), json id = 1)
{
    json j{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.empty())
        j["params"] = params;
    return j.dump();
}

json Handle(McpProtocol& proto, const std::string& body)
{
    std::string out;
    bool called = false;
    proto.HandleMessage(body, [&](std::string s) {
        out = std::move(s);
        called = true;
    });
    CHECK(called); // sync paths must respond before HandleMessage returns
    if (out.empty())
        return json(); // null marks "no response body" (notification -> 202)
    return json::parse(out);
}

void TestInitialize()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("initialize",
                                   json{{"protocolVersion", "2025-11-25"},
                                        {"capabilities", json::object()},
                                        {"clientInfo", {{"name", "test"}, {"version", "0"}}}}));
    CHECK(r["jsonrpc"] == "2.0");
    CHECK(r["id"] == 1);
    CHECK(r["result"]["protocolVersion"] == "2025-11-25");
    CHECK(r["result"]["capabilities"].contains("tools"));
    CHECK(r["result"]["capabilities"].contains("resources"));
    CHECK(r["result"]["serverInfo"]["name"] == "forge");
    CHECK(r["result"]["serverInfo"].contains("version"));
}

void TestInitializedNotificationHasNoResponse()
{
    McpProtocol proto = MakeProtocolWithPing();
    // No "id" => notification => empty response (HTTP layer turns it into 202).
    json r = Handle(proto, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK(r.is_null());
}

void TestPingMethod()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("ping"));
    CHECK(r["result"].is_object());
    CHECK(r["result"].empty());
}

void TestMalformedJsonIsParseError()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, "{not json");
    CHECK(r["error"]["code"] == -32700);
    CHECK(r["id"].is_null());
}

void TestInvalidRequestShapes()
{
    McpProtocol proto = MakeProtocolWithPing();
    // Valid JSON but not a request object.
    CHECK(Handle(proto, "42")["error"]["code"] == -32600);
    // Batch arrays were removed from the MCP spec (2025-06-18) — reject them.
    CHECK(Handle(proto, "[]")["error"]["code"] == -32600);
    // Missing method.
    CHECK(Handle(proto, R"({"jsonrpc":"2.0","id":5})")["error"]["code"] == -32600);
    // Wrong jsonrpc version.
    CHECK(Handle(proto, R"({"jsonrpc":"1.0","id":5,"method":"ping"})")["error"]["code"] == -32600);
    // Non-string jsonrpc/method must not throw out of the kernel (CodeRabbit
    // PR #89: .value() throws type_error on present-but-wrong-type keys).
    CHECK(Handle(proto, R"({"jsonrpc":123,"id":5,"method":"ping"})")["error"]["code"] == -32600);
    CHECK(Handle(proto, R"({"jsonrpc":"2.0","id":5,"method":7})")["error"]["code"] == -32600);
}

void TestUnknownMethod()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("no/such_method"));
    CHECK(r["error"]["code"] == -32601);
    CHECK(r["id"] == 1);
}

void TestToolsList()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("tools/list"));
    const json& tools = r["result"]["tools"];
    CHECK(tools.is_array());
    CHECK(tools.size() == 1);
    CHECK(tools[0]["name"] == "ping");
    CHECK(tools[0]["description"] == "Health check; returns pong.");
    CHECK(tools[0]["inputSchema"]["type"] == "object");
}

void TestToolsCall()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("tools/call", json{{"name", "ping"}, {"arguments", json::object()}}));
    CHECK(r["result"]["content"][0]["type"] == "text");
    CHECK(r["result"]["content"][0]["text"] == "pong");
    CHECK(r["result"]["isError"] == false);
}

void TestToolsCallWithoutArguments()
{
    // "arguments" may be absent for no-arg tools.
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("tools/call", json{{"name", "ping"}}));
    CHECK(r["result"]["content"][0]["text"] == "pong");
}

void TestUnknownToolIsProtocolError()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("tools/call", json{{"name", "nope"}}));
    CHECK(r["error"]["code"] == -32602);
}

void TestMissingToolNameIsProtocolError()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("tools/call"));
    CHECK(r["error"]["code"] == -32602);
}

void TestThrowingToolBecomesIsErrorResult()
{
    // Execution failures are tool results (isError:true), not protocol errors —
    // the model reads the message and self-corrects.
    McpProtocol proto;
    proto.RegisterTool("boom", "Always fails.", json{{"type", "object"}},
                       [](const json&) -> ToolResult { throw std::runtime_error("kaboom"); });
    json r = Handle(proto, Request("tools/call", json{{"name", "boom"}}));
    CHECK(r["result"]["isError"] == true);
    CHECK(r["result"]["content"][0]["text"] == "kaboom");
}

void TestNonFiniteArgPath()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // Clean payloads — including integers and strings — report nothing.
    CHECK(NonFiniteArgPath(json::object()).empty());
    CHECK(NonFiniteArgPath(json{{"distance", 1.5}, {"count", 3}, {"name", "x"}}).empty());

    // Top-level scalar, negative infinity, and float-range overflow: a 1e308
    // double is finite but becomes +inf the moment a handler casts to float.
    CHECK(NonFiniteArgPath(json{{"distance", nan}}) == "distance");
    CHECK(NonFiniteArgPath(json{{"radius", -inf}}) == "radius");
    CHECK(NonFiniteArgPath(json{{"big", 1e308}}) == "big");

    // Nested paths name the offending leaf.
    CHECK(NonFiniteArgPath(json{{"position", {0.0, nan, 0.0}}}) == "position[1]");
    CHECK(NonFiniteArgPath(json{{"params", {{"depth", inf}}}}) == "params.depth");

    // Absurd nesting is refused rather than recursed toward a stack overflow
    // (the wire has no parser-level depth bound).
    json deep = 1.0;
    for (int i = 0; i < 100; ++i)
        deep = json::array({deep});
    CHECK(!NonFiniteArgPath(json{{"a", deep}}).empty());
}

void TestWireCannotSmuggleNonFinite()
{
    // The nlohmann lexer refuses NaN/Inf and double-overflow literals like
    // 1e999 (out_of_range.406 -> parse error). But 1e308 parses cleanly as a
    // finite double and DOES reach dispatch — there the guard is the only
    // wire defence, since the value overflows the float storage every
    // handler uses. The Lua binding path (which really can produce NaN) is
    // tested in test_mcpscript.cpp.
    McpProtocol proto;
    bool ran = false;
    proto.RegisterTool("move", "Records being run.", json{{"type", "object"}},
                       [&](const json&) {
                           ran = true;
                           return ToolResult::Text("ok");
                       });
    json r = Handle(proto, R"({"jsonrpc":"2.0","id":1,"method":"tools/call",)"
                           R"("params":{"name":"move","arguments":{"position":[1e999,0,0]}}})");
    CHECK(r["error"]["code"] == -32700);
    CHECK(!ran);

    r = Handle(proto, R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
                      R"("params":{"name":"move","arguments":{"position":[1e308,0,0]}}})");
    CHECK(r["result"]["isError"] == true);
    const std::string text = r["result"]["content"][0]["text"];
    CHECK(text.find("position[0]") != std::string::npos);
    CHECK(!ran);
}

void TestResourcesListAndRead()
{
    McpProtocol proto = MakeProtocolWithPing();
    proto.RegisterResource("forge://docs/test", "test-doc", "A doc.", "text/markdown",
                           []() { return std::string("# hello"); });

    json list = Handle(proto, Request("resources/list"));
    CHECK(list["result"]["resources"].size() == 1);
    CHECK(list["result"]["resources"][0]["uri"] == "forge://docs/test");
    CHECK(list["result"]["resources"][0]["name"] == "test-doc");
    CHECK(list["result"]["resources"][0]["mimeType"] == "text/markdown");

    json read = Handle(proto, Request("resources/read", json{{"uri", "forge://docs/test"}}));
    CHECK(read["result"]["contents"][0]["uri"] == "forge://docs/test");
    CHECK(read["result"]["contents"][0]["text"] == "# hello");
}

void TestUnknownResourceError()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("resources/read", json{{"uri", "forge://nope"}}));
    CHECK(r["error"]["code"] == -32002); // MCP: resource not found
    CHECK(r["error"]["data"]["uri"] == "forge://nope");
    // A non-string uri must produce an error response, not a throw.
    json bad = Handle(proto, Request("resources/read", json{{"uri", 42}}));
    CHECK(bad.contains("error"));
}

void TestEmptyResourcesList()
{
    McpProtocol proto = MakeProtocolWithPing();
    json r = Handle(proto, Request("resources/list"));
    CHECK(r["result"]["resources"].is_array());
    CHECK(r["result"]["resources"].empty());
}

void TestIdEcho()
{
    McpProtocol proto = MakeProtocolWithPing();
    // String ids must round-trip unchanged.
    json r = Handle(proto, Request("ping", json::object(), "abc-7"));
    CHECK(r["id"] == "abc-7");
}

void TestAsyncToolDeferredResponse()
{
    // Long-running tools (render_image) hold the responder and answer on a
    // later frame; the response must not be produced until they do.
    McpProtocol proto;
    ToolResponder deferred;
    proto.RegisterToolAsync("slow", "Answers later.", json{{"type", "object"}},
                            [&](const json&, ToolResponder respond) { deferred = std::move(respond); });

    std::string out;
    bool called = false;
    proto.HandleMessage(Request("tools/call", json{{"name", "slow"}}, 42),
                        [&](std::string s) {
                            out = std::move(s);
                            called = true;
                        });
    CHECK(!called); // still pending

    deferred(ToolResult::Text("done"));
    CHECK(called);
    json r = json::parse(out);
    CHECK(r["id"] == 42);
    CHECK(r["result"]["content"][0]["text"] == "done");
    CHECK(r["result"]["isError"] == false);
}

void TestAsyncToolThrowBecomesIsError()
{
    // A synchronous throw out of an async handler must still produce a
    // response, or the HTTP request would hang forever.
    McpProtocol proto;
    proto.RegisterToolAsync("bad", "Throws.", json{{"type", "object"}},
                            [](const json&, ToolResponder) -> void { throw std::runtime_error("sync boom"); });
    json r = Handle(proto, Request("tools/call", json{{"name", "bad"}}));
    CHECK(r["result"]["isError"] == true);
    CHECK(r["result"]["content"][0]["text"] == "sync boom");
}

void TestAsyncToolListedInToolsList()
{
    McpProtocol proto;
    proto.RegisterToolAsync("slow", "Answers later.", json{{"type", "object"}},
                            [](const json&, ToolResponder) {});
    json r = Handle(proto, Request("tools/list"));
    CHECK(r["result"]["tools"][0]["name"] == "slow");
}

} // namespace

void RunMcpTests()
{
    TestInitialize();
    TestInitializedNotificationHasNoResponse();
    TestPingMethod();
    TestMalformedJsonIsParseError();
    TestInvalidRequestShapes();
    TestUnknownMethod();
    TestToolsList();
    TestToolsCall();
    TestToolsCallWithoutArguments();
    TestUnknownToolIsProtocolError();
    TestMissingToolNameIsProtocolError();
    TestThrowingToolBecomesIsErrorResult();
    TestNonFiniteArgPath();
    TestWireCannotSmuggleNonFinite();
    TestResourcesListAndRead();
    TestUnknownResourceError();
    TestEmptyResourcesList();
    TestIdEcho();
    TestAsyncToolDeferredResponse();
    TestAsyncToolThrowBecomesIsError();
    TestAsyncToolListedInToolsList();
}

} // namespace forge::test
