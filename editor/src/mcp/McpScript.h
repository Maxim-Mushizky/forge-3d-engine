#pragma once

#include <json.hpp> // nlohmann, bundled with tinygltf

#include <cstdint>
#include <functional>
#include <string>

namespace forge {

// Outcome of one sandboxed script run (#78). The chunk loads under the name
// "script", so failures carry the offending line: "script:12: ...".
struct ScriptResult {
    bool ok = false;
    std::string error;       // set when !ok
    std::string output;      // accumulated print() / forge.print() lines
    nlohmann::json returned; // the script's return value; null when none
};

// A host function exposed to the script as forge.<name>(argsTable). The Lua
// table argument arrives as JSON (named fields -> object, sequences -> array);
// the returned JSON is handed back as a Lua value. Throw std::runtime_error to
// fail the call — the script aborts with "script:<line>: forge.<name>: <what>".
using ScriptFn = std::function<nlohmann::json(const nlohmann::json& args)>;

// Registrar handed to the caller to install forge.* functions before the run.
using ScriptInstall = std::function<void(const std::string& name, ScriptFn fn)>;

// Runs `source` in a fresh sandboxed Lua 5.4 state: base/math/string/table
// libraries only — no os/io/require/load/dofile — plus an instruction budget
// that kills runaway loops and an allocation cap against memory bombs. This
// kernel is GL-free; what the bindings touch is the caller's business.
ScriptResult RunSandboxedScript(const std::string& source,
                                const std::function<void(const ScriptInstall&)>& install,
                                int64_t instructionBudget = 200'000'000);

} // namespace forge
