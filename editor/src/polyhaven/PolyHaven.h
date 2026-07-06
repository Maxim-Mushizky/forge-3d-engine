#pragma once

#include <json.hpp> // nlohmann, bundled with tinygltf

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forge {

// Pure Poly Haven asset-library logic (#84): catalog parsing, client-side
// search, download-file selection and cache-path construction. No network and
// no GL here — WinHTTP lives in net/HttpClient, orchestration in EditorApp —
// so everything in this header is unit-tested in the GL-free suite.

// One catalog entry from GET /assets?type=... .
struct PolyAsset {
    std::string id;   // Poly Haven slug, case-sensitive (e.g. "ArmChair_01")
    std::string name;
    int type = 0; // Poly Haven convention: 0 = hdri, 1 = texture, 2 = model
    std::vector<std::string> categories;
    std::vector<std::string> tags;
    uint64_t downloadCount = 0;
    int maxResX = 0, maxResY = 0;
};

// One downloadable file resolved from GET /files/{id}. relPath is the file's
// location inside the asset's cache directory; model include files keep their
// API-relative subpaths (e.g. "textures/vase_diff_1k.jpg") so glTF buffer and
// texture references resolve.
struct PolyFile {
    std::string url;
    uint64_t size = 0; // bytes, from the API; 0 = unknown
    std::string md5;
    std::string relPath;
};

// The two Poly Haven hosts. The API serves JSON; files come off the CDN.
inline constexpr const char* kPolyHavenApiHost = "api.polyhaven.com";

// GET /assets returns an object keyed by asset id; entries with malformed
// ids or shapes are skipped, a non-object document yields an empty list.
std::vector<PolyAsset> ParseAssetCatalog(const std::string& jsonText);

// The API has no text search — clients filter the catalog. Every whitespace
// token scores independently (id/name substring 3, tag 2, category 1); assets
// matching no token are dropped, ties break on download count. An empty query
// returns the whole catalog by popularity.
std::vector<PolyAsset> SearchAssets(const std::vector<PolyAsset>& catalog,
                                    const std::string& query, size_t limit);

// File selection from a GET /files/{id} document. `resolution` is "1k".."16k";
// when the exact step is missing the largest available below it wins, else the
// smallest above (assets publish different ladders). HDRIs pick the .hdr
// format — stb_image decodes .hdr, not .exr.
std::optional<PolyFile> SelectHdriFile(const nlohmann::json& files,
                                       const std::string& resolution);

// Textures pick jpg (png fallback) for the Diffuse and Rough maps; either may
// be absent. Normal/displacement wait on tangent support (#126).
struct TextureFileSet {
    std::optional<PolyFile> diffuse;
    std::optional<PolyFile> rough;
};
TextureFileSet SelectTextureFiles(const nlohmann::json& files, const std::string& resolution);

// Models: the .gltf file plus every entry of its `include` dict (bin buffer,
// texture images) — the glTF is unloadable without them. Include entries with
// unsafe relative paths poison the whole selection (empty result) rather than
// risking a partial download that parses but renders wrong.
std::vector<PolyFile> SelectModelFiles(const nlohmann::json& files, const std::string& resolution);

// Ids and include paths arrive from the network and end up in filesystem
// paths, so they are allowlisted, never trusted.
bool ValidAssetId(const std::string& id);
// Relative, forward-slash, no "..", no drive letters, conservative charset.
bool SafeRelativePath(const std::string& rel);
// True for "1k".."24k" style tokens.
bool ValidResolution(const std::string& resolution);

struct HttpsUrl {
    std::string host;
    std::string path; // always starts with '/'
};
// Rejects anything but https:// — Poly Haven's CDN 521s plain http anyway.
std::optional<HttpsUrl> SplitHttpsUrl(const std::string& url);

// Cache layout under one base dir (the real one is PolyHavenCacheDir()):
//   <base>/catalog_<apiType>.json      cached GET /assets response
//   <base>/<assetId>/files.json        cached GET /files response
//   <base>/<assetId>/<relPath>         downloaded payload files
std::string CatalogCachePath(const std::string& baseDir, const std::string& apiType);
std::string AssetCacheDir(const std::string& baseDir, const std::string& id);

// %LOCALAPPDATA%/Forge/assets/polyhaven, falling back to a working-directory
// cache when the environment variable is missing.
std::string PolyHavenCacheDir();

} // namespace forge
