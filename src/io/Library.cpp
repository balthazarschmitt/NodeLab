#include "io/Library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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

std::string sidecarPath(const std::string& photoU8) { return photoU8 + ".nlproj"; }

bool hasSidecar(const std::string& photoU8) {
    std::error_code ec;
    return fs::is_regular_file(u8ToPath(sidecarPath(photoU8)), ec);
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

bool readMeta(const std::string& photoU8, Meta& out) {
    out = Meta{};
    nlohmann::json j;
    if (!readJson(sidecarPath(photoU8), j)) return false;
    const nlohmann::json ui = j.value("ui", nlohmann::json::object());
    if (ui.is_object()) out = Meta::fromJson(ui.value("library", nlohmann::json::object()));
    return true;
}

bool writeMeta(const std::string& photoU8, const Meta& m, std::string& err) {
    const std::string side = sidecarPath(photoU8);
    nlohmann::json j;
    if (readJson(side, j)) {
        if (!j["ui"].is_object()) j["ui"] = nlohmann::json::object();
        j["ui"]["library"] = m.toJson();
        return writeJson(side, j, err);
    }
    Graph g;
    defaultGraph(g, photoU8);
    return saveProject(side, g, {{"library", m.toJson()}}, err);
}

void defaultGraph(Graph& g, const std::string& photoU8) {
    g.clear();
    g.colorManagement = ColorManagement::sceneLinear();
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
        g.colorManagement.view = ColorManagement::AgX;
    }
}

nlohmann::json graphFor(const std::string& photoU8, std::string& err) {
    Graph g;
    if (hasSidecar(photoU8)) {
        nlohmann::json ui;
        if (!loadProject(sidecarPath(photoU8), g, ui, err)) return nullptr;
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

bool pasteEdit(const nlohmann::json& graph, const std::string& sourceU8, const std::string& targetU8, std::string& err) {
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
    readMeta(targetU8, m);
    m.edited = true;
    m.thumb.clear();
    nlohmann::json ui = nlohmann::json::object();
    nlohmann::json old;
    if (readJson(sidecarPath(targetU8), old) && old.contains("ui") && old["ui"].is_object()) ui = old["ui"];
    ui["library"] = m.toJson();
    return saveProject(sidecarPath(targetU8), g, ui, err);
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
