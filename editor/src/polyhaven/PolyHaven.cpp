#include "PolyHaven.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace forge {

namespace {

using nlohmann::json;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// "4k" -> 4; 0 when the token isn't a resolution step.
int ResolutionSteps(const std::string& r)
{
    if (r.size() < 2 || r.back() != 'k')
        return 0;
    int v = 0;
    for (size_t i = 0; i + 1 < r.size(); ++i) {
        if (!std::isdigit((unsigned char)r[i]))
            return 0;
        v = v * 10 + (r[i] - '0');
    }
    return v;
}

// Pick the best resolution key present in `node` (an object keyed by "1k",
// "2k", ...): exact match, else largest below the request, else smallest
// above. requireKey filters steps to those that actually carry the wanted
// format (an exr-only step must not shadow an .hdr at another resolution).
// Returns an empty string when no valid key exists.
std::string PickResolution(const json& node, const std::string& requested,
                           const char* requireKey = nullptr)
{
    const int want = ResolutionSteps(requested);
    std::string below, above;
    int belowSteps = 0, aboveSteps = 0;
    std::string exact;
    for (auto it = node.begin(); it != node.end(); ++it) {
        const int steps = ResolutionSteps(it.key());
        if (steps == 0 || !it.value().is_object())
            continue;
        if (requireKey && !it.value().contains(requireKey))
            continue;
        if (steps == want)
            exact = it.key();
        if (steps < want && steps > belowSteps) {
            below = it.key();
            belowSteps = steps;
        }
        if (steps > want && (aboveSteps == 0 || steps < aboveSteps)) {
            above = it.key();
            aboveSteps = steps;
        }
    }
    if (!exact.empty())
        return exact;
    return !below.empty() ? below : above;
}

// Leaf file node {url,size,md5} -> PolyFile named after the URL's basename.
std::optional<PolyFile> FileFromNode(const json& node)
{
    if (!node.is_object() || !node.contains("url") || !node["url"].is_string())
        return std::nullopt;
    PolyFile f;
    f.url = node["url"].get<std::string>();
    if (node.contains("size") && node["size"].is_number_unsigned())
        f.size = node["size"].get<uint64_t>();
    if (node.contains("md5") && node["md5"].is_string())
        f.md5 = node["md5"].get<std::string>();
    const size_t slash = f.url.find_last_of('/');
    f.relPath = slash == std::string::npos ? f.url : f.url.substr(slash + 1);
    if (!SafeRelativePath(f.relPath) || f.relPath.find('/') != std::string::npos)
        return std::nullopt;
    return f;
}

// Map keys in /files are inconsistent case ("Diffuse" vs "nor_gl") — match
// them case-insensitively.
const json* FindMap(const json& files, const char* mapName)
{
    const std::string want = Lower(mapName);
    for (auto it = files.begin(); it != files.end(); ++it)
        if (Lower(it.key()) == want && it.value().is_object())
            return &it.value();
    return nullptr;
}

// Texture maps nest map -> resolution -> format; prefer jpg over png (exr is
// beyond stb_image's LDR path).
std::optional<PolyFile> SelectMapFile(const json& files, const char* mapName,
                                      const std::string& resolution)
{
    const json* map = FindMap(files, mapName);
    if (!map)
        return std::nullopt;
    const std::string res = PickResolution(*map, resolution);
    if (res.empty())
        return std::nullopt;
    const json& formats = (*map)[res];
    for (const char* fmt : {"jpg", "png"})
        if (formats.contains(fmt))
            if (auto f = FileFromNode(formats[fmt]))
                return f;
    return std::nullopt;
}

} // namespace

std::vector<PolyAsset> ParseAssetCatalog(const std::string& jsonText)
{
    std::vector<PolyAsset> out;
    const json doc = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
        return out;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        const json& v = it.value();
        if (!ValidAssetId(it.key()) || !v.is_object())
            continue;
        PolyAsset a;
        a.id = it.key();
        if (v.contains("name") && v["name"].is_string())
            a.name = v["name"].get<std::string>();
        if (v.contains("type") && v["type"].is_number_integer())
            a.type = v["type"].get<int>();
        for (const char* key : {"categories", "tags"}) {
            auto& dst = key[0] == 'c' ? a.categories : a.tags;
            if (v.contains(key) && v[key].is_array())
                for (const json& e : v[key])
                    if (e.is_string())
                        dst.push_back(e.get<std::string>());
        }
        if (v.contains("download_count") && v["download_count"].is_number_unsigned())
            a.downloadCount = v["download_count"].get<uint64_t>();
        if (v.contains("max_resolution") && v["max_resolution"].is_array() &&
            v["max_resolution"].size() == 2 && v["max_resolution"][0].is_number_integer() &&
            v["max_resolution"][1].is_number_integer()) {
            a.maxResX = v["max_resolution"][0].get<int>();
            a.maxResY = v["max_resolution"][1].get<int>();
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<PolyAsset> SearchAssets(const std::vector<PolyAsset>& catalog,
                                    const std::string& query, size_t limit)
{
    // Tokenize once; each token scores independently so "studio hdri" still
    // finds assets that only match "studio".
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : Lower(query) + " ") {
            if (std::isspace((unsigned char)c)) {
                if (!cur.empty())
                    tokens.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    }

    std::vector<std::pair<int, const PolyAsset*>> scored;
    for (const PolyAsset& a : catalog) {
        int score = 0;
        if (tokens.empty()) {
            score = 1; // empty query = browse by popularity
        } else {
            const std::string idName = Lower(a.id) + " " + Lower(a.name);
            for (const std::string& t : tokens) {
                if (idName.find(t) != std::string::npos) {
                    score += 3;
                    continue;
                }
                bool hit = false;
                for (const std::string& tag : a.tags)
                    if (Lower(tag).find(t) != std::string::npos) {
                        score += 2;
                        hit = true;
                        break;
                    }
                if (hit)
                    continue;
                for (const std::string& cat : a.categories)
                    if (Lower(cat).find(t) != std::string::npos) {
                        score += 1;
                        break;
                    }
            }
        }
        if (score > 0)
            scored.push_back({score, &a});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& l, const auto& r) {
                         if (l.first != r.first)
                             return l.first > r.first;
                         return l.second->downloadCount > r.second->downloadCount;
                     });
    std::vector<PolyAsset> out;
    for (const auto& [score, asset] : scored) {
        if (out.size() >= limit)
            break;
        out.push_back(*asset);
    }
    return out;
}

std::optional<PolyFile> SelectHdriFile(const nlohmann::json& files, const std::string& resolution)
{
    if (!files.is_object() || !files.contains("hdri") || !files["hdri"].is_object())
        return std::nullopt;
    const json& ladder = files["hdri"];
    const std::string res = PickResolution(ladder, resolution, "hdr");
    if (res.empty())
        return std::nullopt;
    return FileFromNode(ladder[res]["hdr"]);
}

TextureFileSet SelectTextureFiles(const nlohmann::json& files, const std::string& resolution)
{
    TextureFileSet set;
    if (!files.is_object())
        return set;
    set.diffuse = SelectMapFile(files, "Diffuse", resolution);
    set.rough = SelectMapFile(files, "Rough", resolution);
    return set;
}

std::vector<PolyFile> SelectModelFiles(const nlohmann::json& files, const std::string& resolution)
{
    std::vector<PolyFile> out;
    if (!files.is_object() || !files.contains("gltf") || !files["gltf"].is_object())
        return out;
    const json& ladder = files["gltf"];
    const std::string res = PickResolution(ladder, resolution, "gltf");
    if (res.empty())
        return out;
    const json& main = ladder[res]["gltf"];
    auto mainFile = FileFromNode(main);
    if (!mainFile)
        return out;
    out.push_back(*mainFile);
    // The include dict is mandatory: keys are relative paths the .gltf
    // references (buffer .bin, textures/...), values are file nodes.
    if (main.contains("include") && main["include"].is_object()) {
        for (auto it = main["include"].begin(); it != main["include"].end(); ++it) {
            auto f = FileFromNode(it.value());
            if (!f || !SafeRelativePath(it.key()))
                return {}; // one unsafe entry poisons the download
            f->relPath = it.key();
            out.push_back(std::move(*f));
        }
    }
    return out;
}

bool ValidAssetId(const std::string& id)
{
    if (id.empty() || id.size() > 100)
        return false;
    for (char c : id)
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-')
            return false;
    return true;
}

bool SafeRelativePath(const std::string& rel)
{
    if (rel.empty() || rel.size() > 260 || rel.front() == '/' || rel.back() == '/')
        return false;
    for (char c : rel)
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.' && c != '/')
            return false;
    // Per-segment: nonempty and never "." or ".." — this is what keeps a
    // hostile include key inside the cache directory.
    size_t start = 0;
    while (start <= rel.size()) {
        const size_t end = std::min(rel.find('/', start), rel.size());
        const std::string_view seg(rel.data() + start, end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        start = end + 1;
        if (end == rel.size())
            break;
    }
    return true;
}

bool ValidResolution(const std::string& resolution)
{
    const int steps = ResolutionSteps(resolution);
    return steps >= 1 && steps <= 24;
}

std::optional<HttpsUrl> SplitHttpsUrl(const std::string& url)
{
    constexpr std::string_view prefix = "https://";
    if (url.rfind(prefix.data(), 0) != 0)
        return std::nullopt;
    const size_t hostStart = prefix.size();
    const size_t slash = url.find('/', hostStart);
    HttpsUrl u;
    u.host = url.substr(hostStart, slash == std::string::npos ? std::string::npos
                                                              : slash - hostStart);
    u.path = slash == std::string::npos ? "/" : url.substr(slash);
    if (u.host.empty())
        return std::nullopt;
    for (char c : u.host)
        if (!std::isalnum((unsigned char)c) && c != '.' && c != '-')
            return std::nullopt;
    return u;
}

std::string CatalogCachePath(const std::string& baseDir, const std::string& apiType)
{
    return baseDir + "/catalog_" + apiType + ".json";
}

std::string AssetCacheDir(const std::string& baseDir, const std::string& id)
{
    return baseDir + "/" + id;
}

std::string PolyHavenCacheDir()
{
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData)
        return std::string(localAppData) + "/Forge/assets/polyhaven";
    return "Forge-cache/polyhaven";
}

} // namespace forge
