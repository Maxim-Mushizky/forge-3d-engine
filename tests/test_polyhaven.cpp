#include "test_framework.h"

#include "polyhaven/PolyHaven.h"

#include <string>

// Poly Haven asset-library kernel (#84). The contract: catalog parsing skips
// malformed entries instead of failing wholesale, search ranking is stable and
// popularity-tied, file selection falls back across the published resolution
// ladder, and every network-supplied id/path is validated before it can touch
// a filesystem path.

namespace forge::test {
namespace {

using nlohmann::json;

void TestParseAssetCatalog()
{
    const std::string text = R"({
        "sunny_meadow": {"name": "Sunny Meadow", "type": 0,
            "categories": ["outdoor", "skies"], "tags": ["sun", "field"],
            "download_count": 5000, "max_resolution": [16384, 8192]},
        "red_brick": {"name": "Red Brick", "type": 1,
            "categories": ["brick"], "tags": ["wall", "red"],
            "download_count": 900, "max_resolution": [8192, 8192]},
        "ArmChair_01": {"name": "Arm Chair 01", "type": 2},
        "../evil": {"name": "path traversal id must be skipped", "type": 0},
        "bad_entry": 42
    })";
    auto assets = ParseAssetCatalog(text);
    CHECK(assets.size() == 3); // hostile id + non-object entry skipped

    const PolyAsset* meadow = nullptr;
    const PolyAsset* chair = nullptr;
    for (const PolyAsset& a : assets) {
        if (a.id == "sunny_meadow")
            meadow = &a;
        if (a.id == "ArmChair_01")
            chair = &a;
    }
    CHECK(meadow && chair);
    if (meadow) {
        CHECK(meadow->name == "Sunny Meadow" && meadow->type == 0);
        CHECK(meadow->categories.size() == 2 && meadow->tags.size() == 2);
        CHECK(meadow->downloadCount == 5000);
        CHECK(meadow->maxResX == 16384 && meadow->maxResY == 8192);
    }
    if (chair) // missing optional fields default, mixed-case id survives
        CHECK(chair->downloadCount == 0 && chair->maxResX == 0);

    CHECK(ParseAssetCatalog("not json").empty());
    CHECK(ParseAssetCatalog("[1,2,3]").empty());
}

void TestSearchRankingAndLimit()
{
    std::vector<PolyAsset> catalog;
    auto add = [&catalog](const char* id, const char* name, uint64_t downloads,
                          std::vector<std::string> tags, std::vector<std::string> cats) {
        PolyAsset a;
        a.id = id;
        a.name = name;
        a.downloadCount = downloads;
        a.tags = std::move(tags);
        a.categories = std::move(cats);
        catalog.push_back(std::move(a));
    };
    add("studio_small_03", "Studio Small 03", 100, {"lightbox"}, {"indoor"});
    add("photo_studio_01", "Photo Studio 01", 900, {"softbox"}, {"indoor", "studio"});
    add("meadow", "Meadow", 5000, {"grass"}, {"outdoor"});
    add("garage", "Garage", 50, {"studio light"}, {"indoor"});

    // Name/id substring (3) outranks tag (2); popularity breaks the tie.
    auto hits = SearchAssets(catalog, "studio", 10);
    CHECK(hits.size() == 3);
    if (hits.size() == 3) {
        CHECK(hits[0].id == "photo_studio_01"); // name match, most downloads
        CHECK(hits[1].id == "studio_small_03"); // name match, fewer downloads
        CHECK(hits[2].id == "garage");          // tag-only match ranks below
    }

    // Multi-token: any-token match keeps assets that only hit one word.
    hits = SearchAssets(catalog, "studio hdri", 10);
    CHECK(hits.size() == 3);

    // Empty query = whole catalog by popularity; limit truncates.
    hits = SearchAssets(catalog, "", 2);
    CHECK(hits.size() == 2 && hits[0].id == "meadow");

    CHECK(SearchAssets(catalog, "zebra", 10).empty());
}

void TestSelectHdriFile()
{
    const json files = json::parse(R"({
        "hdri": {
            "1k": {"hdr": {"url": "https://dl.polyhaven.org/x/meadow_1k.hdr",
                           "size": 1000, "md5": "aa"},
                   "exr": {"url": "https://dl.polyhaven.org/x/meadow_1k.exr", "size": 2000}},
            "4k": {"hdr": {"url": "https://dl.polyhaven.org/x/meadow_4k.hdr", "size": 4000}},
            "16k": {"hdr": {"url": "https://dl.polyhaven.org/x/meadow_16k.hdr", "size": 16000}}
        },
        "tonemapped": {"url": "https://dl.polyhaven.org/x/meadow.jpg"}
    })");

    auto exact = SelectHdriFile(files, "4k");
    CHECK(exact.has_value());
    if (exact) { // .hdr picked (stb_image can't read .exr), named from the URL
        CHECK(exact->relPath == "meadow_4k.hdr" && exact->size == 4000);
    }

    // Missing step falls back to the largest below; below the ladder climbs up.
    auto below = SelectHdriFile(files, "8k");
    CHECK(below && below->relPath == "meadow_4k.hdr");
    auto mid = SelectHdriFile(files, "2k");
    CHECK(mid && mid->relPath == "meadow_1k.hdr");

    CHECK(!SelectHdriFile(json::parse(R"({"gltf": {}})"), "4k").has_value());
    CHECK(!SelectHdriFile(json::parse("[1]"), "4k").has_value());
}

void TestSelectTextureFiles()
{
    const json files = json::parse(R"({
        "Diffuse": {"2k": {"png": {"url": "https://dl.polyhaven.org/x/brick_diff_2k.png",
                                   "size": 900}}},
        "Rough": {"2k": {"jpg": {"url": "https://dl.polyhaven.org/x/brick_rough_2k.jpg",
                                 "size": 300},
                         "png": {"url": "https://dl.polyhaven.org/x/brick_rough_2k.png",
                                 "size": 700}}},
        "nor_gl": {"2k": {"jpg": {"url": "https://dl.polyhaven.org/x/brick_nor_2k.jpg"}}}
    })");
    TextureFileSet set = SelectTextureFiles(files, "2k");
    CHECK(set.diffuse && set.diffuse->relPath == "brick_diff_2k.png"); // png fallback
    CHECK(set.rough && set.rough->relPath == "brick_rough_2k.jpg");   // jpg preferred

    // Rough may be absent; Diffuse missing yields an empty slot the caller errors on.
    TextureFileSet none = SelectTextureFiles(json::parse(R"({"nor_gl": {}})"), "2k");
    CHECK(!none.diffuse && !none.rough);
}

void TestSelectModelFilesAndIncludeSafety()
{
    const json files = json::parse(R"({
        "gltf": {"1k": {"gltf": {
            "url": "https://dl.polyhaven.org/x/vase_1k.gltf", "size": 10,
            "include": {
                "vase.bin": {"url": "https://dl.polyhaven.org/x/vase.bin", "size": 20},
                "textures/vase_diff_1k.jpg":
                    {"url": "https://dl.polyhaven.org/x/vase_diff_1k.jpg", "size": 30}
            }}}}
    })");
    auto model = SelectModelFiles(files, "1k");
    CHECK(model.size() == 3);
    if (model.size() == 3) {
        CHECK(model[0].relPath == "vase_1k.gltf");
        // Include keys keep their subpaths so glTF references resolve.
        bool hasTexSubdir = false;
        for (const PolyFile& f : model)
            hasTexSubdir |= f.relPath == "textures/vase_diff_1k.jpg";
        CHECK(hasTexSubdir);
    }

    // One traversal include poisons the whole selection — no partial download.
    json evil = files;
    evil["gltf"]["1k"]["gltf"]["include"]["../../boot.ini"] = {
        {"url", "https://dl.polyhaven.org/x/evil"}, {"size", 1}};
    CHECK(SelectModelFiles(evil, "1k").empty());

    CHECK(SelectModelFiles(json::parse(R"({"hdri": {}})"), "1k").empty());
}

void TestPathAndIdValidation()
{
    CHECK(ValidAssetId("red_brick") && ValidAssetId("ArmChair_01") && ValidAssetId("a-b"));
    CHECK(!ValidAssetId("") && !ValidAssetId("../evil") && !ValidAssetId("a/b"));
    CHECK(!ValidAssetId("a b") && !ValidAssetId(std::string(101, 'a')));

    CHECK(SafeRelativePath("vase.bin"));
    CHECK(SafeRelativePath("textures/vase_diff_1k.jpg"));
    CHECK(!SafeRelativePath("") && !SafeRelativePath("/abs/path"));
    CHECK(!SafeRelativePath("..") && !SafeRelativePath("a/../b") && !SafeRelativePath("a/.."));
    CHECK(!SafeRelativePath("C:/windows/system32") && !SafeRelativePath("a\\b"));
    CHECK(!SafeRelativePath("a//b") && !SafeRelativePath("a/") && !SafeRelativePath("./a"));

    CHECK(ValidResolution("1k") && ValidResolution("8k") && ValidResolution("24k"));
    CHECK(!ValidResolution("") && !ValidResolution("k") && !ValidResolution("2K"));
    CHECK(!ValidResolution("2048") && !ValidResolution("2k4") && !ValidResolution("99k"));
}

void TestSplitHttpsUrl()
{
    auto u = SplitHttpsUrl("https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/x_1k.hdr");
    CHECK(u.has_value());
    if (u) {
        CHECK(u->host == "dl.polyhaven.org");
        CHECK(u->path == "/file/ph-assets/HDRIs/hdr/1k/x_1k.hdr");
    }
    auto bare = SplitHttpsUrl("https://api.polyhaven.com");
    CHECK(bare && bare->path == "/");

    CHECK(!SplitHttpsUrl("http://dl.polyhaven.org/x").has_value()); // https only
    CHECK(!SplitHttpsUrl("file:///etc/passwd").has_value());
    CHECK(!SplitHttpsUrl("https:///nohost").has_value());
    CHECK(!SplitHttpsUrl("https://evil host/x").has_value());
}

void TestCachePaths()
{
    CHECK(CatalogCachePath("base", "hdris") == "base/catalog_hdris.json");
    CHECK(AssetCacheDir("base", "red_brick") == "base/red_brick");
    // The real base dir is env-dependent; just pin the suffix contract.
    const std::string dir = PolyHavenCacheDir();
    CHECK(dir.size() >= 9 && dir.substr(dir.size() - 9) == "polyhaven");
}

} // namespace

void RunPolyHavenTests()
{
    TestParseAssetCatalog();
    TestSearchRankingAndLimit();
    TestSelectHdriFile();
    TestSelectTextureFiles();
    TestSelectModelFilesAndIncludeSafety();
    TestPathAndIdValidation();
    TestSplitHttpsUrl();
    TestCachePaths();
}

} // namespace forge::test
