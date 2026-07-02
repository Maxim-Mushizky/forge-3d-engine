#include "Settings.h"

#include <json.hpp> // nlohmann, bundled with tinygltf

namespace forge {

using nlohmann::json;

// Read a typed value if present and of the right type; otherwise keep `out`.
// Hand-edited files get field-level forgiveness instead of all-or-nothing.
template <typename T>
static void Get(const json& section, const char* key, T& out)
{
    if (!section.is_object() || !section.contains(key))
        return;
    const json& v = section[key];
    if constexpr (std::is_same_v<T, bool>) {
        if (v.is_boolean())
            out = v.get<bool>();
    } else if constexpr (std::is_same_v<T, int>) {
        if (v.is_number_integer())
            out = v.get<int>();
    } else {
        if (v.is_number())
            out = v.get<float>();
    }
}

std::string SettingsToJson(const Settings& s, const std::string& existingJson)
{
    // Start from the previous file so keys this build doesn't know (a newer
    // build's settings) survive the rewrite.
    json j = json::parse(existingJson, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object())
        j = json::object();

    j["version"] = 1;

    json& cam = j["camera"];
    cam["orbitSensitivity"] = s.orbitSensitivity;
    cam["zoomSpeed"] = s.zoomSpeed;
    cam["panSpeed"] = s.panSpeed;
    cam["invertOrbitY"] = s.invertOrbitY;

    json& vp = j["viewport"];
    vp["defaultFov"] = s.defaultFov;
    vp["snapEnabled"] = s.snapEnabled;
    vp["snapTranslate"] = s.snapTranslate;
    vp["snapRotateDeg"] = s.snapRotateDeg;
    vp["snapScale"] = s.snapScale;

    json& rt = j["rendering"];
    rt["bounces"] = s.rtBounces;
    rt["scale"] = s.rtScale;
    rt["denoise"] = s.denoise;
    rt["denoiseStrength"] = s.denoiseStrength;

    json& files = j["files"];
    files["recentFilesMax"] = s.recentFilesMax;
    files["exportScale"] = s.exportScale;

    j["appearance"]["fontScale"] = s.fontScale;
    j["interface"]["showTooltips"] = s.showTooltips;
    j["interface"]["mcpEnabled"] = s.mcpEnabled;
    j["interface"]["mcpPort"] = s.mcpPort;

    return j.dump(2); // pretty-printed: the file is meant to be hand-editable
}

Settings SettingsFromJson(const std::string& jsonText)
{
    Settings s; // defaults; every miss below leaves the default in place

    json j = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object())
        return s;

    const json& cam = j.value("camera", json::object());
    Get(cam, "orbitSensitivity", s.orbitSensitivity);
    Get(cam, "zoomSpeed", s.zoomSpeed);
    Get(cam, "panSpeed", s.panSpeed);
    Get(cam, "invertOrbitY", s.invertOrbitY);

    const json& vp = j.value("viewport", json::object());
    Get(vp, "defaultFov", s.defaultFov);
    Get(vp, "snapEnabled", s.snapEnabled);
    Get(vp, "snapTranslate", s.snapTranslate);
    Get(vp, "snapRotateDeg", s.snapRotateDeg);
    Get(vp, "snapScale", s.snapScale);

    const json& rt = j.value("rendering", json::object());
    Get(rt, "bounces", s.rtBounces);
    Get(rt, "scale", s.rtScale);
    Get(rt, "denoise", s.denoise);
    Get(rt, "denoiseStrength", s.denoiseStrength);

    const json& files = j.value("files", json::object());
    Get(files, "recentFilesMax", s.recentFilesMax);
    Get(files, "exportScale", s.exportScale);

    Get(j.value("appearance", json::object()), "fontScale", s.fontScale);
    const json& ui = j.value("interface", json::object());
    Get(ui, "showTooltips", s.showTooltips);
    Get(ui, "mcpEnabled", s.mcpEnabled);
    Get(ui, "mcpPort", s.mcpPort);

    return s;
}

} // namespace forge
