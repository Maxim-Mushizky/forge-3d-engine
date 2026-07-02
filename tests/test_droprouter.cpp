#include "test_framework.h"

#include "DropRouter.h"

#include <string>

namespace forge::test {

namespace {

void TestModelExtensions()
{
    CHECK(ClassifyDrop("C:/assets/Duck.glb") == DropAction::ImportModel);
    CHECK(ClassifyDrop("C:/assets/scene.gltf") == DropAction::ImportModel);
    CHECK(ClassifyDrop("C:/assets/teapot.obj") == DropAction::ImportModel);
}

void TestHdriExtension()
{
    CHECK(ClassifyDrop("C:/skies/studio.hdr") == DropAction::LoadHdri);
}

void TestImageExtensions()
{
    CHECK(ClassifyDrop("C:/tex/wood.png") == DropAction::AssignTexture);
    CHECK(ClassifyDrop("C:/tex/brick.jpg") == DropAction::AssignTexture);
    CHECK(ClassifyDrop("C:/tex/rust.jpeg") == DropAction::AssignTexture);
}

void TestSceneExtension()
{
    CHECK(ClassifyDrop("C:/projects/house.forge") == DropAction::OpenScene);
}

void TestStlIsCalledOut()
{
    // STL gets its own action so the UI can explain the workaround (#2).
    CHECK(ClassifyDrop("C:/prints/bracket.stl") == DropAction::UnsupportedStl);
}

void TestCaseInsensitive()
{
    CHECK(ClassifyDrop("C:/assets/DUCK.GLB") == DropAction::ImportModel);
    CHECK(ClassifyDrop("C:/tex/Wood.PNG") == DropAction::AssignTexture);
    CHECK(ClassifyDrop("C:/skies/Sky.HDR") == DropAction::LoadHdri);
    CHECK(ClassifyDrop("C:/p/House.Forge") == DropAction::OpenScene);
}

void TestUnknownAndEdgeCases()
{
    CHECK(ClassifyDrop("C:/docs/readme.txt") == DropAction::Unknown);
    CHECK(ClassifyDrop("C:/bin/noextension") == DropAction::Unknown);
    CHECK(ClassifyDrop("C:/weird/trailingdot.") == DropAction::Unknown);
    CHECK(ClassifyDrop("") == DropAction::Unknown);
    // A dot in a directory name must not be mistaken for an extension.
    CHECK(ClassifyDrop("C:/my.assets/model") == DropAction::Unknown);
    // Only the last extension counts.
    CHECK(ClassifyDrop("C:/tex/wood.png.bak") == DropAction::Unknown);
}

} // namespace

void RunDropRouterTests()
{
    TestModelExtensions();
    TestHdriExtension();
    TestImageExtensions();
    TestSceneExtension();
    TestStlIsCalledOut();
    TestCaseInsensitive();
    TestUnknownAndEdgeCases();
}

} // namespace forge::test
