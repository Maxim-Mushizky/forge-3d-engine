#pragma once

#include <string>

namespace forge {

// User preferences (#13): one struct, one JSON file (forge_settings.json).
// Fields are defaults applied at startup; the settings window edits them live.
// Plain data so the whole thing copies for "restore defaults" per tab.
struct Settings {
    // --- camera --------------------------------------------------------
    float orbitSensitivity = 0.006f; // radians per pixel of mouse drag
    float zoomSpeed = 0.1f;          // distance fraction per wheel notch
    float panSpeed = 0.0015f;        // world units per pixel per unit distance
    bool invertOrbitY = false;

    // --- viewport ------------------------------------------------------
    float defaultFov = 45.0f; // degrees, applied to new sessions
    bool snapEnabled = false;
    float snapTranslate = 0.25f; // world units per grid step
    float snapRotateDeg = 15.0f; // degrees per angle step
    float snapScale = 0.1f;      // scale-factor step

    // --- rendering (RT defaults for a new session) -----------------------
    int rtBounces = 4;
    float rtScale = 0.75f; // render-resolution fraction
    bool denoise = true;
    float denoiseStrength = 0.7f;

    // --- files -----------------------------------------------------------
    int recentFilesMax = 8;
    float exportScale = 100.0f; // mm per scene unit (STL)

    // --- appearance ------------------------------------------------------
    float fontScale = 1.0f; // multiplies ImGui's global font scale

    // --- interface -------------------------------------------------------
    bool showTooltips = true;
    bool mcpEnabled = false; // embedded MCP server (#75); off by default
    int mcpPort = 8765;      // localhost-only listen port
};

// JSON round-trip is pure (no file I/O) so it's unit-testable headless.
// existingJson: previous file contents; unknown keys in it are preserved so a
// newer build's settings survive a save from an older one. Pass "" when there
// is no previous file.
std::string SettingsToJson(const Settings& s, const std::string& existingJson);

// Missing keys and type mismatches fall back to defaults field-by-field;
// malformed input yields all defaults. Never throws.
Settings SettingsFromJson(const std::string& jsonText);

} // namespace forge
