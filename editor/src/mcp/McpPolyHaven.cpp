#include "EditorApp.h"

#include "net/HttpClient.h"
#include "polyhaven/PolyHaven.h"

#include <forge/core/Log.h>
#include <forge/renderer/TextureSource.h>

#include <json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

// Poly Haven asset-library tools (#84): search_polyhaven + download_polyhaven_
// asset. The worker thread touches network and disk only; every GL-side effect
// (HDRI load, texture creation, model import) happens on the main thread in
// UpdatePolyHaven — same split as render_image. One job runs at a time.

namespace forge {

using nlohmann::json;

namespace {

// Poly Haven's ToS asks for an identifying User-Agent so they can attribute
// traffic. Assets are CC0; this is etiquette, not auth.
constexpr const char* kUserAgent = "ForgeEditor/1.0 (https://github.com/Maxim-Mushizky/forge-3d-engine)";

// Downloads above this step get large fast (a 16k HDRI is ~200 MB); the sky
// and PT sampling gain nothing past 8k at our render sizes.
constexpr int kMaxResolutionSteps = 8;

ToolResult Err(std::string msg) { return ToolResult::Text(std::move(msg), /*error=*/true); }

// error_handler_t::replace: cache paths inherit the ANSI-codepage bytes of
// %LOCALAPPDATA% (non-ASCII usernames), which are not valid UTF-8 — a strict
// dump would throw right in the pump.
ToolResult JsonResult(const json& j)
{
    return ToolResult::Text(j.dump(2, ' ', false, json::error_handler_t::replace));
}

std::string ReadFileText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool WriteFileBytes(const std::string& path, const void* data, size_t bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write((const char*)data, (std::streamsize)bytes);
    return out.good();
}

// Singular tool arg -> Poly Haven API type; empty = invalid.
std::string ApiType(const std::string& type)
{
    if (type == "hdri")
        return "hdris";
    if (type == "texture")
        return "textures";
    if (type == "model")
        return "models";
    return {};
}

std::string TransportError(const HttpResult& r)
{
    return r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
}

// Fetch one download-file to disk unless a size-matching copy is already
// cached. Returns empty on success, an error message otherwise.
std::string FetchFile(const PolyFile& f, const std::string& destPath, bool& wasCached)
{
    std::error_code ec;
    const uint64_t onDisk = std::filesystem::file_size(destPath, ec);
    if (!ec && (f.size == 0 || onDisk == f.size)) {
        wasCached = true;
        return {}; // cached copy verified by size — works offline
    }
    wasCached = false;
    const auto url = SplitHttpsUrl(f.url);
    if (!url)
        return "refusing non-https download url: " + f.url;
    // One retry: multi-file model downloads hit transient CDN timeouts, and
    // failing the whole batch for one blip wastes the files already fetched.
    HttpResult res = HttpsGet(url->host, url->path, kUserAgent);
    if (!res.Ok())
        res = HttpsGet(url->host, url->path, kUserAgent);
    if (!res.Ok())
        return "download failed for " + f.relPath + ": " + TransportError(res);
    if (f.size != 0 && res.body.size() != f.size)
        return "size mismatch for " + f.relPath + " (got " + std::to_string(res.body.size()) +
               ", expected " + std::to_string(f.size) + ") — retry, or the asset changed upstream";
    if (!WriteFileBytes(destPath, res.body.data(), res.body.size()))
        return "couldn't write " + destPath;
    return {};
}

} // namespace

void EditorApp::StartPolyHavenSearch(const json& args, ToolResponder respond)
{
    if (m_PolyJob.active) {
        respond(Err("A Poly Haven request is already in progress"));
        return;
    }
    const std::string apiType = ApiType(args.value("type", ""));
    if (apiType.empty()) {
        respond(Err("type must be 'hdri', 'texture' or 'model'"));
        return;
    }
    const std::string query = args.value("query", "");
    const size_t limit = (size_t)std::clamp(args.value("limit", 15), 1, 50);

    // `active` is set LAST: if the json copy or the thread constructor throws,
    // McpProtocol answers the error and the job must not be left wedged.
    m_PolyJob.done = false;
    m_PolyJob.respond = std::move(respond);
    if (m_PolyJob.worker.joinable())
        m_PolyJob.worker.join(); // finished long ago; pump already consumed it

    m_PolyJob.worker = std::thread([job = &m_PolyJob, apiType, query, limit] {
        try {
            const std::string catalogPath = CatalogCachePath(PolyHavenCacheDir(), apiType);

            // Serve from the disk cache while it's fresh; the catalog changes
            // rarely and the response is ~1 MB. Parse-validated before use —
            // a cached garbage body must not brick search for the whole TTL.
            std::vector<PolyAsset> catalog;
            std::error_code ec;
            const auto mtime = std::filesystem::last_write_time(catalogPath, ec);
            if (!ec &&
                std::filesystem::file_time_type::clock::now() - mtime < std::chrono::hours(1))
                catalog = ParseAssetCatalog(ReadFileText(catalogPath));

            bool stale = false;
            if (catalog.empty()) {
                const HttpResult res =
                    HttpsGet(kPolyHavenApiHost, "/assets?type=" + apiType, kUserAgent);
                if (res.Ok()) {
                    catalog = ParseAssetCatalog(
                        std::string(res.body.begin(), res.body.end()));
                    if (catalog.empty()) {
                        // 200 with a garbage body (CDN error page): don't cache it.
                        job->error = "Poly Haven catalog for '" + apiType +
                                     "' parsed empty — the API answered with something "
                                     "unexpected; retry later";
                        job->done = true;
                        return;
                    }
                    if (!WriteFileBytes(catalogPath, res.body.data(), res.body.size()))
                        FORGE_WARN("Couldn't cache Poly Haven catalog at %s",
                                   catalogPath.c_str());
                } else {
                    // Any-age cache beats nothing when offline.
                    catalog = ParseAssetCatalog(ReadFileText(catalogPath));
                    stale = !catalog.empty();
                    if (catalog.empty()) {
                        job->error = "Poly Haven catalog request failed (" +
                                     TransportError(res) + ") and no cached catalog for '" +
                                     apiType + "' — check the connection and retry";
                        job->done = true;
                        return;
                    }
                }
            }

            json hits = json::array();
            for (const PolyAsset& a : SearchAssets(catalog, query, limit))
                hits.push_back({{"id", a.id},
                                {"name", a.name},
                                {"categories", a.categories},
                                {"tags", a.tags},
                                {"downloads", a.downloadCount},
                                {"maxResolution", {a.maxResX, a.maxResY}}});
            job->result = json{{"assets", std::move(hits)}, {"catalogSize", catalog.size()}};
            if (stale)
                job->result["stale"] = true; // offline: served from an aged cache
        } catch (const std::exception& e) {
            // A throw on this thread (filesystem conversions, bad_alloc) would
            // otherwise std::terminate the editor.
            job->error = std::string("Poly Haven search failed: ") + e.what();
        }
        job->done = true;
    });
    m_PolyJob.active = true;
}

void EditorApp::StartPolyHavenDownload(const json& args, ToolResponder respond)
{
    if (m_PolyJob.active) {
        respond(Err("A Poly Haven request is already in progress"));
        return;
    }
    const std::string asset = args.value("asset", "");
    if (!ValidAssetId(asset)) {
        respond(Err("Provide a valid asset id from search_polyhaven"));
        return;
    }
    const std::string type = args.value("type", "");
    if (ApiType(type).empty()) {
        respond(Err("type must be 'hdri', 'texture' or 'model'"));
        return;
    }
    const std::string resolution = args.value("resolution", "2k");
    if (!ValidResolution(resolution)) {
        respond(Err("resolution must look like '1k'..'8k'"));
        return;
    }
    if (std::stoi(resolution) > kMaxResolutionSteps) { // digits validated above
        respond(Err("resolution is capped at 8k"));
        return;
    }

    // `active` last, same reasoning as StartPolyHavenSearch.
    m_PolyJob.done = false;
    m_PolyJob.respond = std::move(respond);
    m_PolyJob.applyKind = type;
    m_PolyJob.applyArgs = args;
    if (m_PolyJob.worker.joinable())
        m_PolyJob.worker.join();

    m_PolyJob.worker = std::thread([job = &m_PolyJob, asset, type, resolution] {
      try {
        const std::string dir = AssetCacheDir(PolyHavenCacheDir(), asset);
        const std::string filesPath = dir + "/files.json";

        // The /files listing is cached next to the payload so a previously
        // downloaded asset re-applies fully offline.
        std::string filesText;
        {
            const HttpResult res = HttpsGet(kPolyHavenApiHost, "/files/" + asset, kUserAgent);
            if (res.Ok()) {
                filesText.assign(res.body.begin(), res.body.end());
                if (!WriteFileBytes(filesPath, res.body.data(), res.body.size()))
                    FORGE_WARN("Couldn't cache Poly Haven file listing at %s", filesPath.c_str());
            } else {
                filesText = ReadFileText(filesPath);
                if (filesText.empty()) {
                    job->error = "Poly Haven file listing failed (" + TransportError(res) +
                                 ") and no cached listing for '" + asset +
                                 "' — bad asset id, or offline without a prior download";
                    job->done = true;
                    return;
                }
            }
        }
        const json files = json::parse(filesText, nullptr, /*allow_exceptions=*/false);
        if (files.is_discarded() || !files.is_object()) {
            job->error = "Malformed file listing for '" + asset +
                         "' — wrong asset id, or the API answered with an error page";
            job->done = true;
            return;
        }

        // Resolve which files this asset type needs at the requested step.
        std::vector<PolyFile> wanted;
        std::string roughRel;
        if (type == "hdri") {
            if (auto f = SelectHdriFile(files, resolution))
                wanted.push_back(std::move(*f));
            if (wanted.empty()) {
                job->error = "'" + asset + "' has no .hdr file at any resolution — is it an HDRI?";
                job->done = true;
                return;
            }
        } else if (type == "texture") {
            TextureFileSet set = SelectTextureFiles(files, resolution);
            if (!set.diffuse) {
                job->error = "'" + asset + "' has no Diffuse map — is it a texture asset?";
                job->done = true;
                return;
            }
            wanted.push_back(std::move(*set.diffuse));
            if (set.rough) {
                roughRel = set.rough->relPath;
                wanted.push_back(std::move(*set.rough));
            }
        } else { // model
            wanted = SelectModelFiles(files, resolution);
            if (wanted.empty()) {
                job->error = "'" + asset +
                             "' has no glTF download (or its file list is unsafe) — is it a model?";
                job->done = true;
                return;
            }
        }

        json fileReport = json::array();
        for (const PolyFile& f : wanted) {
            if (job->cancel.load()) { // editor shutting down: stop between files
                job->error = "Download cancelled";
                job->done = true;
                return;
            }
            const std::string dest = dir + "/" + f.relPath;
            bool cached = false;
            const std::string err = FetchFile(f, dest, cached);
            if (!err.empty()) {
                job->error = err;
                job->done = true;
                return;
            }
            fileReport.push_back({{"path", dest}, {"bytes", f.size}, {"cached", cached}});
        }

        // First wanted file is the one the main thread applies: the .hdr, the
        // Diffuse map, or the .gltf (include files only support it).
        std::error_code ec;
        const std::string mainDest = dir + "/" + wanted.front().relPath;
        job->applyPath = std::filesystem::absolute(mainDest, ec).lexically_normal().generic_string();
        if (ec)
            job->applyPath = mainDest;
        if (!roughRel.empty()) {
            job->applyRoughPath =
                std::filesystem::absolute(dir + "/" + roughRel, ec).lexically_normal().generic_string();
            if (ec)
                job->applyRoughPath = dir + "/" + roughRel;
        }
        job->result = json{{"asset", asset},
                           {"type", type},
                           {"resolution", resolution},
                           {"files", std::move(fileReport)}};
      } catch (const std::exception& e) {
          // A throw on this thread would std::terminate the editor.
          job->error = std::string("Poly Haven download failed: ") + e.what();
      }
      job->done = true;
    });
    m_PolyJob.active = true;
}

// GL-side application of a finished download. Runs on the main thread only;
// mutates job.result/job.error, the caller responds.
void EditorApp::ApplyPolyHavenDownload()
{
    PolyHavenJob& job = m_PolyJob;
    const std::string asset = job.result.value("asset", "");
    const std::string resolution = job.result.value("resolution", "");
    bool applied = false; // provenance is only for assets the scene actually uses

    if (job.applyKind == "hdri") {
        if (!LoadHDRIFile(job.applyPath)) {
            job.error = "Downloaded but couldn't load HDRI: " + job.applyPath;
            return;
        }
        job.result["applied"] = json{{"environment", job.applyPath}};
        applied = true;
    } else if (job.applyKind == "texture") {
        if (job.applyArgs.contains("id") || job.applyArgs.contains("name")) {
            // Resolve the target like FindToolTarget does (id string, name
            // fallback), then apply BOTH maps in one snapshot: validate-then-
            // mutate, one undo entry, no albedo-without-roughness half-state.
            Entity* e = nullptr;
            if (job.applyArgs.contains("id") && job.applyArgs["id"].is_string()) {
                const std::string idStr = job.applyArgs["id"];
                try {
                    e = m_Scene.Find(std::stoull(idStr));
                } catch (const std::exception&) {
                    e = nullptr;
                }
                if (!e) {
                    job.error = "Downloaded, but no entity with id " + idStr;
                    return;
                }
            } else if (job.applyArgs["name"].is_string()) {
                const std::string name = job.applyArgs["name"];
                for (Entity& cand : m_Scene.Entities())
                    if (cand.name == name) {
                        e = &cand; // first match, same tie-break as FindToolTarget
                        break;
                    }
                if (!e) {
                    job.error = "Downloaded, but no entity named \"" + name + "\"";
                    return;
                }
            } else {
                job.error = "Downloaded, but id/name must be strings";
                return;
            }
            int slot = 0;
            if (job.applyArgs.contains("materialSlot")) {
                if (!job.applyArgs["materialSlot"].is_number_integer()) {
                    job.error = "materialSlot must be an integer";
                    return;
                }
                slot = job.applyArgs["materialSlot"].get<int>();
                if (slot < 0 || slot >= (int)MaterialSlotCount(*e)) {
                    job.error = "materialSlot " + std::to_string(slot) +
                                " out of range: entity has " +
                                std::to_string(MaterialSlotCount(*e)) + " material slot(s)";
                    return;
                }
            }
            const std::string albedoSource = "file:" + job.applyPath;
            auto albedoTex = TextureFromSource(albedoSource, TextureChannel::Albedo);
            if (!albedoTex) {
                job.error = "Downloaded, but couldn't decode the albedo map: " + job.applyPath;
                return;
            }
            std::shared_ptr<Texture2D> roughTex;
            std::string mrSource;
            if (!job.applyRoughPath.empty()) {
                mrSource = "file:" + job.applyRoughPath;
                roughTex = TextureFromSource(mrSource, TextureChannel::Roughness);
                if (!roughTex) {
                    job.error =
                        "Downloaded, but couldn't decode the roughness map: " + job.applyRoughPath;
                    return; // nothing mutated yet — the scene stays untouched
                }
            }
            Entity before = *e;
            Material& m = MaterialForSlot(*e, (uint32_t)slot);
            m.albedoMap = albedoTex;
            m.albedoSource = albedoSource;
            if (roughTex) {
                m.metallicRoughnessMap = roughTex;
                m.mrSource = mrSource;
            }
            m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
            json appliedJson = json{{"albedo", job.applyPath}};
            if (roughTex)
                appliedJson["roughness"] = job.applyRoughPath;
            job.result["applied"] = std::move(appliedJson);
            applied = true;
        } else {
            // No target entity: hand the agent the cached paths instead.
            job.result["hint"] =
                "Pass id/name to apply directly, or call set_texture with the returned paths";
        }
    } else if (job.applyKind == "model") {
        if (!ImportModel(job.applyPath)) {
            job.error = "Downloaded but couldn't import glTF: " + job.applyPath;
            return;
        }
        job.result["applied"] = json{{"imported", job.applyPath}};
        applied = true;
    }

    // Record provenance once per asset — CC0 needs no attribution, but the
    // scene should say which assets it actually uses.
    if (applied && !asset.empty()) {
        bool known = false;
        for (const PolyProvenance& p : m_PolyProvenance)
            known |= p.id == asset;
        if (!known)
            m_PolyProvenance.push_back({asset, job.applyKind, resolution});
    }
}

void EditorApp::UpdatePolyHaven()
{
    if (!m_PolyJob.active || !m_PolyJob.done.load())
        return;
    if (m_PolyJob.worker.joinable())
        m_PolyJob.worker.join(); // happens-before: worker writes are visible now

    ToolResponder respond = std::move(m_PolyJob.respond);
    // Nothing above Run() catches: a throw out of the apply or the responder
    // (json copies, GL wrapper failures) must not std::terminate the editor.
    try {
        if (!m_PolyJob.error.empty()) {
            respond(Err(m_PolyJob.error));
        } else if (m_PolyJob.applyKind.empty()) {
            respond(JsonResult(m_PolyJob.result)); // search: nothing to apply
        } else {
            ApplyPolyHavenDownload();
            respond(m_PolyJob.error.empty() ? JsonResult(m_PolyJob.result)
                                            : Err(m_PolyJob.error));
        }
    } catch (const std::exception& e) {
        FORGE_ERROR("Poly Haven apply/respond threw: %s", e.what());
        try {
            respond(Err(std::string("Poly Haven request failed: ") + e.what()));
        } catch (...) {
            // Responder itself unusable; the HTTP side times out the request.
        }
    }

    m_PolyJob.active = false;
    m_PolyJob.done = false;
    m_PolyJob.result = json();
    m_PolyJob.error.clear();
    m_PolyJob.applyKind.clear();
    m_PolyJob.applyPath.clear();
    m_PolyJob.applyRoughPath.clear();
    m_PolyJob.applyArgs = json();
}

} // namespace forge
