#include "test_framework.h"

#include "Settings.h"

#include <string>

namespace forge::test {

namespace {

bool Near(float a, float b) { return a > b - 1e-4f && a < b + 1e-4f; }

void TestDefaultsRoundTrip()
{
    Settings s;
    Settings back = SettingsFromJson(SettingsToJson(s, ""));
    CHECK(Near(back.orbitSensitivity, s.orbitSensitivity));
    CHECK(Near(back.zoomSpeed, s.zoomSpeed));
    CHECK(Near(back.panSpeed, s.panSpeed));
    CHECK(back.invertOrbitY == s.invertOrbitY);
    CHECK(Near(back.defaultFov, s.defaultFov));
    CHECK(back.snapEnabled == s.snapEnabled);
    CHECK(Near(back.snapTranslate, s.snapTranslate));
    CHECK(Near(back.snapRotateDeg, s.snapRotateDeg));
    CHECK(Near(back.snapScale, s.snapScale));
    CHECK(back.rtBounces == s.rtBounces);
    CHECK(Near(back.rtScale, s.rtScale));
    CHECK(back.denoise == s.denoise);
    CHECK(Near(back.denoiseStrength, s.denoiseStrength));
    CHECK(back.recentFilesMax == s.recentFilesMax);
    CHECK(Near(back.exportScale, s.exportScale));
    CHECK(Near(back.fontScale, s.fontScale));
    CHECK(back.showTooltips == s.showTooltips);
}

void TestModifiedValuesRoundTrip()
{
    Settings s;
    s.orbitSensitivity = 0.012f;
    s.invertOrbitY = true;
    s.snapEnabled = true;
    s.snapTranslate = 0.5f;
    s.rtBounces = 8;
    s.denoise = false;
    s.recentFilesMax = 3;
    s.fontScale = 1.25f;
    s.showTooltips = false;

    Settings back = SettingsFromJson(SettingsToJson(s, ""));
    CHECK(Near(back.orbitSensitivity, 0.012f));
    CHECK(back.invertOrbitY == true);
    CHECK(back.snapEnabled == true);
    CHECK(Near(back.snapTranslate, 0.5f));
    CHECK(back.rtBounces == 8);
    CHECK(back.denoise == false);
    CHECK(back.recentFilesMax == 3);
    CHECK(Near(back.fontScale, 1.25f));
    CHECK(back.showTooltips == false);
}

void TestMissingKeysFallBackToDefaults()
{
    // A hand-edited file that only sets one value keeps defaults elsewhere.
    Settings back = SettingsFromJson(R"({"camera":{"orbitSensitivity":0.02}})");
    CHECK(Near(back.orbitSensitivity, 0.02f));
    CHECK(Near(back.zoomSpeed, Settings{}.zoomSpeed));
    CHECK(back.rtBounces == Settings{}.rtBounces);
}

void TestMalformedJsonFallsBackToDefaults()
{
    Settings a = SettingsFromJson("not json at all {{{");
    CHECK(a.rtBounces == Settings{}.rtBounces);
    Settings b = SettingsFromJson("");
    CHECK(Near(b.defaultFov, Settings{}.defaultFov));
}

void TestUnknownKeysPreservedOnRewrite()
{
    // Forward compatibility: a newer build's keys survive a save from this one.
    std::string existing = R"({"futureSection":{"answer":42},"camera":{"customKey":true}})";
    Settings s;
    s.orbitSensitivity = 0.01f;
    std::string out = SettingsToJson(s, existing);

    CHECK(out.find("futureSection") != std::string::npos);
    CHECK(out.find("customKey") != std::string::npos);
    // And the known key we set is written too.
    Settings back = SettingsFromJson(out);
    CHECK(Near(back.orbitSensitivity, 0.01f));
}

void TestWrongTypesIgnored()
{
    // A string where a number belongs must not crash or corrupt other keys.
    Settings back = SettingsFromJson(R"({"camera":{"orbitSensitivity":"fast"},"rendering":{"bounces":6}})");
    CHECK(Near(back.orbitSensitivity, Settings{}.orbitSensitivity));
    CHECK(back.rtBounces == 6);
}

} // namespace

void RunSettingsTests()
{
    TestDefaultsRoundTrip();
    TestModifiedValuesRoundTrip();
    TestMissingKeysFallBackToDefaults();
    TestMalformedJsonFallsBackToDefaults();
    TestUnknownKeysPreservedOnRewrite();
    TestWrongTypesIgnored();
}

} // namespace forge::test
