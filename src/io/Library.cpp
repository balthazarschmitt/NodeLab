#include "io/Library.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "core/ColorManagement.h"
#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "io/RawDecode.h"
#include "nodes/io/IONodes.h"

namespace fs = std::filesystem;

namespace library {

namespace {

constexpr const char* kDenoise = "filter.denoise";
constexpr const char* kBasic = "color.basic";

void setParam(Node& n, const char* name, const nlohmann::json& v) {
    const auto& ps = n.info().params;
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].name == name) n.params[i] = v;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool samePath(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (fs::equivalent(u8ToPath(a), u8ToPath(b), ec)) return true;
    return lower(pathToU8(u8ToPath(a).lexically_normal())) == lower(pathToU8(u8ToPath(b).lexically_normal()));
}

bool readJson(const std::string& pathU8, nlohmann::json& j) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) return false;
    try {
        j = nlohmann::json::parse(f);
    } catch (const std::exception&) {
        return false;
    }
    return j.is_object() && j.value("app", "") == "NodeLab";
}

// Like saveProject: a temp file renamed over the old one, so a crash never leaves half a file.
bool writeJson(const std::string& pathU8, const nlohmann::json& j, std::string& err) {
    const fs::path path = u8ToPath(pathU8);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << j.dump(2))) {
            err = "cannot write " + pathToU8(path.filename());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        err = ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

nlohmann::json Meta::toJson() const {
    nlohmann::json j = {{"rating", rating}, {"flag", flag}, {"edited", edited}};
    if (!thumb.empty()) j["thumb"] = thumb;
    return j;
}

Meta Meta::fromJson(const nlohmann::json& j) {
    Meta m;
    if (!j.is_object()) return m;
    try {
        m.rating = std::clamp(j.value("rating", 0), 0, 5);
        m.flag = std::clamp(j.value("flag", 0), -1, 1);
        m.edited = j.value("edited", false);
        m.thumb = j.value("thumb", std::string());
    } catch (const std::exception&) {
        m = Meta{};
    }
    return m;
}

std::string sidecarPath(const std::string& photoU8, int copy) {
    return copy > 0 ? photoU8 + ".copy" + std::to_string(copy) + ".nlproj" : photoU8 + ".nlproj";
}

bool hasSidecar(const std::string& photoU8, int copy) {
    std::error_code ec;
    return fs::is_regular_file(u8ToPath(sidecarPath(photoU8, copy)), ec);
}

std::vector<std::string> listFolder(const std::string& dirU8) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(u8ToPath(dirU8), ec)) {
        std::error_code fec;
        if (!e.is_regular_file(fec)) continue;
        const std::string p = pathToU8(e.path());
        if (isImageFile(p)) out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string la = lower(pathToU8(u8ToPath(a).filename())), lb = lower(pathToU8(u8ToPath(b).filename()));
        return la != lb ? la < lb : a < b;
    });
    return out;
}

std::vector<Entry> listEntries(const std::string& dirU8) {
    const std::vector<std::string> photos = listFolder(dirU8);
    // Virtual copies' sidecars, by the lower-case file name of their photo.
    std::map<std::string, std::vector<int>> copies;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(u8ToPath(dirU8), ec)) {
        const std::string name = pathToU8(e.path().filename());
        constexpr std::string_view ext = ".nlproj";
        if (name.size() <= ext.size() || lower(name.substr(name.size() - ext.size())) != ext) continue;
        const std::string stem = name.substr(0, name.size() - ext.size());  // photo.ext.copyN
        const size_t dot = stem.rfind('.');
        if (dot == std::string::npos || lower(stem.substr(dot + 1, 4)) != "copy") continue;
        const std::string digits = stem.substr(dot + 5);
        if (digits.empty() || digits.size() > 6 || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        if (const int n = std::stoi(digits); n > 0) copies[lower(stem.substr(0, dot))].push_back(n);
    }
    std::vector<Entry> out;
    for (const std::string& p : photos) {
        out.push_back({p, 0});
        auto it = copies.find(lower(pathToU8(u8ToPath(p).filename())));
        if (it == copies.end()) continue;
        std::sort(it->second.begin(), it->second.end());
        for (int n : it->second) out.push_back({p, n});
    }
    return out;
}

int createVirtualCopy(const std::string& photoU8, int fromCopy, std::string& err) {
    int n = 1;
    while (hasSidecar(photoU8, n)) ++n;
    nlohmann::json j;
    if (hasSidecar(photoU8, fromCopy)) {
        if (!readJson(sidecarPath(photoU8, fromCopy), j)) {
            err = pathToU8(u8ToPath(sidecarPath(photoU8, fromCopy)).filename()) + " can't be read";
            return -1;
        }
        return writeJson(sidecarPath(photoU8, n), j, err) ? n : -1;
    }
    Graph g;
    defaultGraph(g, photoU8);
    return saveProject(sidecarPath(photoU8, n), g, {{"library", Meta{}.toJson()}}, err) ? n : -1;
}

namespace {
std::atomic<bool> gRecycle{true};
}

void setUseRecycleBin(bool on) { gRecycle = on; }

bool removeVirtualCopy(const std::string& photoU8, int copy, std::string& err) {
    if (copy <= 0) {
        err = "only virtual copies can be removed";
        return false;
    }
    const fs::path side = u8ToPath(sidecarPath(photoU8, copy));
#ifdef _WIN32
    // To the Recycle Bin, so a copy removed by mistake can be brought back.
    if (gRecycle) {
        std::wstring from = side.wstring();
        from.push_back(L'\0');  // the list ends with two NULs
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) return true;
        err = "could not remove " + pathToU8(side.filename());
        return false;
    }
#endif
    std::error_code ec;
    if (fs::remove(side, ec)) return true;
    err = ec ? ec.message() : "no such file";
    return false;
}

bool readMeta(const std::string& photoU8, Meta& out, int copy) {
    out = Meta{};
    nlohmann::json j;
    if (!readJson(sidecarPath(photoU8, copy), j)) return false;
    const nlohmann::json ui = j.value("ui", nlohmann::json::object());
    if (ui.is_object()) out = Meta::fromJson(ui.value("library", nlohmann::json::object()));
    return true;
}

bool writeMeta(const std::string& photoU8, const Meta& m, std::string& err, int copy) {
    const std::string side = sidecarPath(photoU8, copy);
    if (!hasSidecar(photoU8, copy)) {
        Graph g;
        defaultGraph(g, photoU8);
        return saveProject(side, g, {{"library", m.toJson()}}, err);
    }
    // A sidecar that exists but can't be read is never replaced: it holds an edit. A sync client
    // or a virus scanner may hold it open for a moment, so try again briefly; a damaged one stays
    // as it is for the user to repair, and the rating isn't saved.
    nlohmann::json j;
    for (int attempt = 0; !readJson(side, j); ++attempt) {
        if (attempt == 3) {
            err = pathToU8(u8ToPath(side).filename()) + " can't be read, so it was left unchanged";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!j["ui"].is_object()) j["ui"] = nlohmann::json::object();
    j["ui"]["library"] = m.toJson();
    return writeJson(side, j, err);
}

namespace {
int gDefaultView = ColorManagement::Standard, gDefaultLook = ColorManagement::None;
}

void setDefaultView(int view, int look) {
    gDefaultView = view;
    gDefaultLook = look;
}

void defaultGraph(Graph& g, const std::string& photoU8) {
    g.clear();
    g.colorManagement = ColorManagement::sceneLinear();
    g.colorManagement.view = gDefaultView;
    g.colorManagement.look = gDefaultLook;
    Node* in = g.addNode(ImageInputNode::staticInfo().type, 40, 80);
    Node* dn = g.addNode(kDenoise, 300, 80);
    Node* basic = g.addNode(kBasic, 540, 80);
    Node* out = g.addNode(OutputNode::staticInfo().type, 800, 80);
    // As if chosen in the UI, so a RAW gets the RAW defaults (Baseline Exposure).
    static_cast<ImageInputNode&>(*in).chooseFile(photoU8);
    g.connect(in->id, 0, dn->id, 0);
    g.connect(dn->id, 0, basic->id, 0);
    g.connect(basic->id, 0, out->id, 0);
    if (raw::isRawPath(photoU8)) {
        setParam(*dn, "Color", 25.0f);
        if (g.colorManagement.view != ColorManagement::AgX) g.colorManagement.look = ColorManagement::None;
        g.colorManagement.view = ColorManagement::AgX;
    }
}

nlohmann::json graphFor(const std::string& photoU8, std::string& err, int copy) {
    Graph g;
    if (hasSidecar(photoU8, copy)) {
        nlohmann::json ui;
        if (!loadProject(sidecarPath(photoU8, copy), g, ui, err)) return nullptr;
    } else {
        defaultGraph(g, photoU8);
    }
    return g.toJson();
}

bool retargetEdit(Graph& g, const std::string& sourceU8, const std::string& targetU8) {
    ImageInputNode* input = nullptr;
    for (const auto& [id, n] : g.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type) {
            auto* in = static_cast<ImageInputNode*>(n.get());
            if (samePath(in->paramS(0), sourceU8)) {
                input = in;
                break;
            }
            if (!input) input = in;
        }
    if (!input) return false;
    input->chooseFile(targetU8);
    return true;
}

bool pasteEdit(const nlohmann::json& graph, const std::string& sourceU8, const std::string& targetU8, std::string& err,
               int targetCopy) {
    Graph g;
    try {
        g.fromJson(graph);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    if (!retargetEdit(g, sourceU8, targetU8)) {
        err = "the edit has no Image Input";
        return false;
    }
    Meta m;
    readMeta(targetU8, m, targetCopy);
    m.edited = true;
    m.thumb.clear();
    nlohmann::json ui = nlohmann::json::object();
    nlohmann::json old;
    if (readJson(sidecarPath(targetU8, targetCopy), old) && old.contains("ui") && old["ui"].is_object()) ui = old["ui"];
    ui["library"] = m.toJson();
    return saveProject(sidecarPath(targetU8, targetCopy), g, ui, err);
}

ImagePtr loadThumbnail(const std::string& photoU8, int edge, std::string& err) {
    ImagePtr img;
    if (raw::isRawPath(photoU8)) img = raw::loadThumbnail(photoU8, edge, err);
    if (!img) {
        // Upright (EXIF), values as stored: display-encoded already.
        DecodeOptions d;
        d.sceneLinear = true;
        img = loadImage(photoU8, err, d, true);
        // A RAW without an embedded preview: its half-size decode is linear; show it through sRGB.
        if (img && raw::isRawPath(photoU8)) img = colormgmt::displayImage(img, ColorManagement::sceneLinear());
    }
    return img ? downscaleToFit(img, edge) : nullptr;
}

ImagePtr renderThumbnail(const Graph& g, int edge, std::string& err) {
    const int outId = g.firstOfType(OutputNode::staticInfo().type);
    if (!outId) {
        err = "no Output node";
        return nullptr;
    }
    try {
        ImageCache cache;
        EvalContext ctx;
        ctx.proxy = true;
        ctx.proxyEdge = std::max(edge * 2, 512);  // a little oversampled, then box-filtered down
        ctx.cache = &cache;
        initContextSize(g, ctx);
        Evaluator ev;
        ev.releaseIntermediates = true;
        ImagePtr img = ev.evaluateDisplay(g, outId, ctx);
        if (!img) {
            err = "the Output node has no input";
            return nullptr;
        }
        img = downscaleToFit(img, edge);
        return colormgmt::displayImage(img, g.colorManagement);
    } catch (const std::exception& e) {
        err = e.what();
        return nullptr;
    }
}

std::string encodeThumb(const Image& img) { return base64Encode(encodeJpegMemory(img, 85)); }

ImagePtr decodeThumb(const std::string& b64) {
    const std::vector<unsigned char> bytes = base64Decode(b64);
    if (bytes.empty()) return nullptr;
    std::string err;
    return decodeImageMemory(bytes.data(), bytes.size(), err);
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<unsigned char>& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t v = uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? uint32_t(bytes[i + 2]) : 0);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < bytes.size() ? kB64[v >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? kB64[v & 63] : '=';
    }
    return out;
}

std::vector<unsigned char> base64Decode(const std::string& text) {
    int map[256];
    std::fill(std::begin(map), std::end(map), -1);
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(kB64[i])] = i;
    std::vector<unsigned char> out;
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : text) {
        if (c == '=') break;
        const int v = map[c];
        if (v < 0) continue;  // whitespace, line breaks
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>(acc >> bits & 0xFF));
        }
    }
    return out;
}

}  // namespace library
