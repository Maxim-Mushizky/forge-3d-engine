#include "McpScript.h"

// Lua is compiled as C++ (see cmake/Dependencies.cmake), so its errors unwind
// as exceptions and RAII below stays safe. Include the plain headers — not
// lua.hpp's extern "C" wrapper — or the declarations wouldn't mangle to match.
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>

namespace forge {

using nlohmann::json;

namespace {

// The hook fires every stride instructions and burns it off the budget —
// coarse enough to be free, fine enough that a runaway loop dies in ~ms.
constexpr int kHookStride = 100000;
constexpr size_t kMemoryCap = 256u * 1024u * 1024u; // string.rep bombs stop here

// Per-run state, reachable from the C callbacks via the state's extra space.
struct RunCtx {
    std::string output;
    std::vector<std::pair<std::string, ScriptFn>> fns;
    int64_t instructionsLeft = 0;
    size_t memoryUsed = 0;
};

RunCtx*& CtxSlot(lua_State* L)
{
    return *static_cast<RunCtx**>(lua_getextraspace(L));
}

// Allocator with a hard cap: Lua treats a null return as out-of-memory and
// raises, which pcall turns into a script error instead of an editor OOM.
void* CappedAlloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    RunCtx* ctx = static_cast<RunCtx*>(ud);
    const size_t old = ptr ? osize : 0; // osize is a type tag when ptr is null
    if (nsize == 0) {
        ctx->memoryUsed -= old;
        std::free(ptr);
        return nullptr;
    }
    if (ctx->memoryUsed - old + nsize > kMemoryCap)
        return nullptr;
    void* grown = std::realloc(ptr, nsize);
    if (grown)
        ctx->memoryUsed = ctx->memoryUsed - old + nsize;
    return grown;
}

void Hook(lua_State* L, lua_Debug*)
{
    RunCtx* ctx = CtxSlot(L);
    ctx->instructionsLeft -= kHookStride;
    if (ctx->instructionsLeft <= 0)
        luaL_error(L, "instruction budget exceeded (runaway loop?)");
}

// --- json <-> lua ---------------------------------------------------------

void JsonToLua(lua_State* L, const json& v)
{
    switch (v.type()) {
    case json::value_t::boolean:
        lua_pushboolean(L, v.get<bool>());
        break;
    case json::value_t::number_integer:
        lua_pushinteger(L, (lua_Integer)v.get<int64_t>());
        break;
    case json::value_t::number_unsigned:
        lua_pushinteger(L, (lua_Integer)v.get<uint64_t>());
        break;
    case json::value_t::number_float:
        lua_pushnumber(L, v.get<double>());
        break;
    case json::value_t::string: {
        const std::string& s = v.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
        break;
    }
    case json::value_t::array:
        lua_createtable(L, (int)v.size(), 0);
        for (int i = 0; i < (int)v.size(); ++i) {
            JsonToLua(L, v[i]);
            lua_rawseti(L, -2, i + 1);
        }
        break;
    case json::value_t::object:
        lua_createtable(L, 0, (int)v.size());
        for (const auto& [key, value] : v.items()) {
            JsonToLua(L, value);
            lua_setfield(L, -2, key.c_str());
        }
        break;
    default: // null / discarded
        lua_pushnil(L);
        break;
    }
}

json LuaToJson(lua_State* L, int idx, int depth = 0)
{
    if (depth > 32)
        throw std::runtime_error("table nested too deep (reference cycle?)");
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return nullptr;
    case LUA_TBOOLEAN:
        return (bool)lua_toboolean(L, idx);
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            return (int64_t)lua_tointeger(L, idx);
        return lua_tonumber(L, idx);
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        return std::string(s, len);
    }
    case LUA_TTABLE: {
        // A table is an array when its keys are exactly 1..n; anything else
        // (string keys, holes) serializes as an object with stringified keys.
        const lua_Unsigned n = lua_rawlen(L, idx);
        lua_Unsigned count = 0;
        bool isArray = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            ++count;
            if (!(lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 &&
                  (lua_Unsigned)lua_tointeger(L, -2) <= n))
                isArray = false;
            lua_pop(L, 1);
        }
        if (isArray && count == n) {
            json arr = json::array();
            for (lua_Unsigned i = 1; i <= n; ++i) {
                lua_rawgeti(L, idx, (lua_Integer)i);
                arr.push_back(LuaToJson(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return arr;
        }
        json obj = json::object();
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            std::string key;
            if (lua_type(L, -2) == LUA_TSTRING) {
                key = lua_tostring(L, -2);
            } else {
                lua_pushvalue(L, -2); // tostring on a copy: lua_next needs the
                key = luaL_tolstring(L, -1, nullptr); // real key unmutated
                lua_pop(L, 2);
            }
            obj[key] = LuaToJson(L, -1, depth + 1);
            lua_pop(L, 1);
        }
        return obj;
    }
    default:
        throw std::runtime_error(std::string("cannot convert a ") +
                                 lua_typename(L, lua_type(L, idx)) + " to JSON");
    }
}

// --- C entry points -------------------------------------------------------

int Print(lua_State* L)
{
    RunCtx* ctx = CtxSlot(L);
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len); // honours __tostring
        if (i > 1)
            ctx->output += '\t';
        ctx->output.append(s, len);
        lua_pop(L, 1);
    }
    ctx->output += '\n';
    return 0;
}

// Trampoline for every forge.* binding; upvalue 1 = index into RunCtx::fns.
int CallBinding(lua_State* L)
{
    RunCtx* ctx = CtxSlot(L);
    const auto& [name, fn] = ctx->fns[(size_t)lua_tointeger(L, lua_upvalueindex(1))];
    std::string err;
    try {
        json args = json::object();
        if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
            if (lua_type(L, 1) != LUA_TTABLE)
                throw std::runtime_error("expects one table of named arguments");
            args = LuaToJson(L, 1);
            if (args.is_array() && args.empty())
                args = json::object(); // {} converts as an empty array
            if (!args.is_object())
                throw std::runtime_error("expects named (string-keyed) arguments");
        }
        JsonToLua(L, fn(args));
        return 1;
    } catch (const std::exception& ex) {
        err = "forge." + name + ": " + ex.what();
    }
    // luaL_error copies the message and prefixes the script line. Raised
    // outside the catch so the exception being thrown isn't a rethrow-in-
    // handler; err's destructor runs during the C++ unwind (Lua-as-C++).
    return luaL_error(L, "%s", err.c_str());
}

struct LuaStateOwner {
    lua_State* L = nullptr;
    ~LuaStateOwner()
    {
        if (L)
            lua_close(L);
    }
};

} // namespace

ScriptResult RunSandboxedScript(const std::string& source,
                                const std::function<void(const ScriptInstall&)>& install,
                                int64_t instructionBudget)
{
    ScriptResult result;
    RunCtx ctx;
    ctx.instructionsLeft = instructionBudget;

    LuaStateOwner owner;
    owner.L = lua_newstate(CappedAlloc, &ctx);
    lua_State* L = owner.L;
    if (!L) {
        result.error = "failed to create Lua state";
        return result;
    }
    CtxSlot(L) = &ctx;

    // Sandbox: only the pure-computation libraries exist. os/io/package are
    // never opened; the base library's file/chunk loaders are stripped.
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 4);
    for (const char* gate : {"dofile", "loadfile", "load", "require"}) {
        lua_pushnil(L);
        lua_setglobal(L, gate);
    }
    lua_pushcfunction(L, Print);
    lua_setglobal(L, "print"); // print goes to the tool result, not stdout

    install([&ctx](const std::string& name, ScriptFn fn) {
        ctx.fns.emplace_back(name, std::move(fn));
    });
    lua_createtable(L, 0, (int)ctx.fns.size() + 1);
    for (size_t i = 0; i < ctx.fns.size(); ++i) {
        lua_pushinteger(L, (lua_Integer)i);
        lua_pushcclosure(L, CallBinding, 1);
        lua_setfield(L, -2, ctx.fns[i].first.c_str());
    }
    lua_pushcfunction(L, Print);
    lua_setfield(L, -2, "print"); // forge.print, alias of print
    lua_setglobal(L, "forge");

    lua_sethook(L, Hook, LUA_MASKCOUNT, kHookStride);

    // "@" marks the chunk as named source, so errors read "script:12: ..."
    // instead of the noisier [string "..."] form.
    if (luaL_loadbufferx(L, source.data(), source.size(), "@script", "t") != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        result.error = msg ? msg : "syntax error";
        return result;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        size_t len = 0;
        const char* msg = luaL_tolstring(L, -1, &len);
        result.error.assign(msg, len);
        result.output = std::move(ctx.output);
        return result;
    }
    try {
        if (!lua_isnil(L, -1))
            result.returned = LuaToJson(L, -1);
    } catch (const std::exception& ex) {
        result.error = std::string("return value: ") + ex.what();
        result.output = std::move(ctx.output);
        return result;
    }
    result.ok = true;
    result.output = std::move(ctx.output);
    return result;
}

} // namespace forge
