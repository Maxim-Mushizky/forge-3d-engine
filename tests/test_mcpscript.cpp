#include "test_framework.h"

#include "mcp/McpProtocol.h" // NonFiniteArgPath — the bindings' front-door guard
#include "mcp/McpScript.h"

#include <json.hpp>

#include <string>
#include <vector>

// Suites for the sandboxed Lua kernel (#78). The scene bindings are the
// editor's business; here the host functions are stubs, which is exactly the
// GL-free seam RunSandboxedScript was cut along.

namespace forge::test {

using nlohmann::json;
using ScriptLog = std::vector<json>;

// Runs source with an `echo` binding (records + returns its args) and a
// `boom` binding (always throws).
static ScriptResult RunWithStubs(const std::string& source, ScriptLog* log = nullptr,
                                 int64_t budget = 200'000'000)
{
    return RunSandboxedScript(
        source,
        [&](const ScriptInstall& add) {
            add("echo", [log](const json& args) {
                if (log)
                    log->push_back(args);
                return args;
            });
            add("boom", [](const json&) -> json {
                throw std::runtime_error("kaput");
            });
        },
        budget);
}

static void ReturnValues()
{
    ScriptResult r = RunWithStubs("return 42");
    CHECK(r.ok);
    CHECK(r.returned == json(42));

    r = RunWithStubs("return {1, 2, 3}");
    CHECK(r.ok);
    CHECK(r.returned == json({1, 2, 3}));

    r = RunWithStubs("return {name = 'snowman', parts = 3}");
    CHECK(r.ok);
    CHECK(r.returned["name"] == "snowman");
    CHECK(r.returned["parts"] == 3);

    r = RunWithStubs("return {pos = {1.5, 0, -2}}"); // nested array survives
    CHECK(r.ok);
    CHECK(r.returned["pos"] == json({1.5, 0, -2}));

    r = RunWithStubs("local x = 1"); // no return -> null
    CHECK(r.ok);
    CHECK(r.returned.is_null());
}

static void PrintAccumulates()
{
    ScriptResult r = RunWithStubs("print('a', 1) forge.print('b')");
    CHECK(r.ok);
    CHECK(r.output == "a\t1\nb\n");
}

static void SandboxBlocksEscapes()
{
    // Dangerous globals must read as nil — a script can prove it.
    ScriptResult r = RunWithStubs("return os == nil and io == nil and require == nil "
                                  "and load == nil and dofile == nil and loadfile == nil "
                                  "and package == nil and debug == nil");
    CHECK(r.ok);
    CHECK(r.returned == json(true));

    // The safe libraries do exist.
    r = RunWithStubs("return math.floor(3.7) + #('abc') + #({1,2})");
    CHECK(r.ok);
    CHECK(r.returned == json(3 + 3 + 2));
}

static void SyntaxErrorHasLine()
{
    ScriptResult r = RunWithStubs("local x = 1\nlocal = nope");
    CHECK(!r.ok);
    CHECK(r.error.find("script:2") != std::string::npos);
}

static void RuntimeErrorHasLine()
{
    ScriptResult r = RunWithStubs("local x = 1\n\nerror('busted')");
    CHECK(!r.ok);
    CHECK(r.error.find("script:3") != std::string::npos);
    CHECK(r.error.find("busted") != std::string::npos);
}

static void BindingErrorAbortsScript()
{
    ScriptLog log;
    ScriptResult r = RunWithStubs("forge.echo{n = 1}\nforge.boom{}\nforge.echo{n = 2}", &log);
    CHECK(!r.ok);
    CHECK(r.error.find("script:2") != std::string::npos);
    CHECK(r.error.find("forge.boom") != std::string::npos);
    CHECK(r.error.find("kaput") != std::string::npos);
    CHECK(log.size() == 1); // nothing ran past the failure
    CHECK(log[0]["n"] == 1);

    // ...but a script may probe with pcall and continue.
    log.clear();
    r = RunWithStubs("local ok = pcall(forge.boom)\nforge.echo{ok = ok}", &log);
    CHECK(r.ok);
    CHECK(log.size() == 1);
    CHECK(log[0]["ok"] == false);
}

static void BindingArgsRoundTrip()
{
    ScriptLog log;
    ScriptResult r = RunWithStubs(
        "local e = forge.echo{primitive = 'cube', position = {0, 1.5, 0}, count = 3, flat = true}\n"
        "return e.position[2]",
        &log);
    CHECK(r.ok);
    CHECK(log.size() == 1);
    CHECK(log[0]["primitive"] == "cube");
    CHECK(log[0]["position"] == json({0, 1.5, 0}));
    CHECK(log[0]["count"] == 3);
    CHECK(log[0]["flat"] == true);
    CHECK(r.returned == json(1.5)); // result table indexes like the input

    // No argument and empty table both reach the host as an empty object.
    log.clear();
    r = RunWithStubs("forge.echo() forge.echo{}", &log);
    CHECK(r.ok);
    CHECK(log.size() == 2);
    CHECK(log[0].is_object() && log[0].empty());
    CHECK(log[1].is_object() && log[1].empty());

    // A non-table argument is a binding error, not a crash.
    r = RunWithStubs("forge.echo('cube')");
    CHECK(!r.ok);
    CHECK(r.error.find("named arguments") != std::string::npos);
}

static void InstructionBudgetAborts()
{
    ScriptResult r = RunWithStubs("while true do end", nullptr, /*budget=*/500'000);
    CHECK(!r.ok);
    CHECK(r.error.find("instruction budget") != std::string::npos);

    // A loop that fits the budget still completes.
    r = RunWithStubs("local s = 0 for i = 1, 100 do s = s + i end return s", nullptr,
                     /*budget=*/500'000);
    CHECK(r.ok);
    CHECK(r.returned == json(5050));
}

static void BudgetSurvivesProtectedCalls()
{
    // pcall/xpcall must not swallow the kill switch: the wrapped versions
    // re-raise after the protected call if the budget tripped inside it.
    ScriptLog log;
    ScriptResult r = RunWithStubs("local ok = pcall(function() while true do end end)\n"
                                  "forge.echo{caught = ok}",
                                  &log, /*budget=*/500'000);
    CHECK(!r.ok);
    CHECK(r.error.find("instruction budget") != std::string::npos);
    CHECK(log.empty()); // nothing runs past the re-raise

    r = RunWithStubs("local ok = xpcall(function() while true do end end,\n"
                     "                  function(e) return e end)\n"
                     "forge.echo{caught = ok}",
                     &log, /*budget=*/500'000);
    CHECK(!r.ok);
    CHECK(r.error.find("instruction budget") != std::string::npos);
    CHECK(log.empty());

    // Ordinary errors stay catchable through the wrappers.
    r = RunWithStubs("local ok, err = pcall(function() error('soft') end)\n"
                     "return {ok = ok, err = err}");
    CHECK(r.ok);
    CHECK(r.returned["ok"] == false);
    CHECK(r.returned["err"].get<std::string>().find("soft") != std::string::npos);
}

static void MemoryCapAborts()
{
    // Doubling string.rep would hit gigabytes fast; the capped allocator must
    // turn it into a script error, not an editor OOM.
    ScriptResult r = RunWithStubs("local s = 'x'\nwhile true do s = s .. s end");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

static void NonFiniteArgsRejectedAtBridge()
{
    // Mirrors the guard execute_script wraps around every forge.* binding:
    // NonFiniteArgPath runs before the handler. A Lua 0/0 or math.huge must
    // round-trip through LuaToJson into the rejection path with the handler
    // never entered (#104).
    bool reached = false;
    auto run = [&](const std::string& src) {
        reached = false;
        return RunSandboxedScript(src, [&](const ScriptInstall& add) {
            add("apply", [&](const json& args) -> json {
                if (const std::string bad = NonFiniteArgPath(args); !bad.empty())
                    throw std::runtime_error("argument '" + bad + "' is NaN or infinite");
                reached = true;
                return json{{"ok", true}};
            });
        });
    };

    ScriptResult r = run("forge.apply{distance = 0/0}");
    CHECK(!r.ok);
    CHECK(r.error.find("distance") != std::string::npos);
    CHECK(r.error.find("NaN or infinite") != std::string::npos);
    CHECK(!reached);

    r = run("forge.apply{position = {0/0, 0, 0}}");
    CHECK(!r.ok);
    CHECK(r.error.find("position[0]") != std::string::npos);
    CHECK(!reached);

    r = run("forge.apply{radius = math.huge}");
    CHECK(!r.ok);
    CHECK(r.error.find("radius") != std::string::npos);
    CHECK(!reached);

    // Ordinary finite arguments still go through.
    r = run("forge.apply{distance = 1.5, position = {0, 2, 0}}");
    CHECK(r.ok);
    CHECK(reached);
}

static void DeepNestingIsSafe()
{
    // Depth ~64 nesting in every direction must fail as a clean script error
    // (depth cap / explicit stack growth), never corrupt the Lua stack: a C
    // function is only guaranteed 20 free slots (#162).

    // Deep table as a binding argument (LuaToJson).
    ScriptLog log;
    ScriptResult r = RunWithStubs("local t = {leaf = true}\n"
                                  "for i = 1, 64 do t = {inner = t} end\n"
                                  "forge.echo{payload = t}",
                                  &log);
    CHECK(!r.ok);
    CHECK(r.error.find("nested too deep") != std::string::npos);
    CHECK(log.empty()); // handler never entered

    // Deep table as the script's return value (LuaToJson outside pcall).
    r = RunWithStubs("local t = {}\nfor i = 1, 64 do t = {inner = t} end\nreturn t");
    CHECK(!r.ok);
    CHECK(r.error.find("nested too deep") != std::string::npos);

    // Deep JSON coming back from a host function (JsonToLua).
    r = RunSandboxedScript("forge.deep{}", [&](const ScriptInstall& add) {
        add("deep", [](const json&) {
            json v = 1;
            for (int i = 0; i < 64; ++i)
                v = json{{"inner", v}};
            return v;
        });
    });
    CHECK(!r.ok);
    CHECK(r.error.find("nested too deep") != std::string::npos);

    // Within the cap, deep values round-trip intact (exercises stack growth).
    r = RunWithStubs("local t = {leaf = 7}\n"
                     "for i = 1, 20 do t = {inner = t} end\n"
                     "local c = forge.echo{payload = t}.payload\n"
                     "for i = 1, 20 do c = c.inner end\n"
                     "return c.leaf");
    CHECK(r.ok);
    CHECK(r.returned == json(7));
}

static void PrintOutputCapped()
{
    // print() accumulates on the host heap, outside the Lua allocator cap, so
    // it carries its own 4 MB budget; the script keeps running past it (#162).
    ScriptResult r = RunWithStubs("local s = string.rep('x', 65536)\n"
                                  "for i = 1, 80 do print(s) end\n"
                                  "return 'done'");
    CHECK(r.ok);
    CHECK(r.returned == json("done"));
    CHECK(r.output.size() <= 4u * 1024u * 1024u + 64u);
    CHECK(r.output.find("truncated") != std::string::npos);
}

static void ParametricBuildLoop()
{
    // The shape of a real agent script: a loop of spawns with computed
    // transforms. 24 binding calls from one script.
    ScriptLog log;
    ScriptResult r = RunWithStubs(
        "for i = 0, 23 do\n"
        "  local a = i * math.pi / 12\n"
        "  forge.echo{name = 'post ' .. i, position = {3 * math.cos(a), 0.5, 3 * math.sin(a)}}\n"
        "end\n"
        "return 24",
        &log);
    CHECK(r.ok);
    CHECK(r.returned == json(24));
    CHECK(log.size() == 24);
    CHECK(log[6]["name"] == "post 6");
    // post 6 sits a quarter-turn around: x ~ 0, z ~ 3.
    CHECK(ApproxEq((float)log[6]["position"][0].get<double>(), 0.0f, 1e-4f));
    CHECK(ApproxEq((float)log[6]["position"][2].get<double>(), 3.0f, 1e-4f));
}

void RunMcpScriptTests()
{
    ReturnValues();
    PrintAccumulates();
    SandboxBlocksEscapes();
    SyntaxErrorHasLine();
    RuntimeErrorHasLine();
    BindingErrorAbortsScript();
    BindingArgsRoundTrip();
    InstructionBudgetAborts();
    BudgetSurvivesProtectedCalls();
    MemoryCapAborts();
    NonFiniteArgsRejectedAtBridge();
    DeepNestingIsSafe();
    PrintOutputCapped();
    ParametricBuildLoop();
    std::printf("[ok] mcp script kernel tests\n");
}

} // namespace forge::test
