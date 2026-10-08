#include "io/Library.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
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
    return j.is_object() && isProjectApp(j.value("app", ""));
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

const char* labelName(int label) {
    static const char* const names[] = {"None", "Red", "Yellow", "Green", "Blue", "Purple"};
    return label >= 0 && label < kLabelCount ? names[label] : names[0];
}

namespace {
// Text for XML: the five special characters escaped, and control characters XML 1.0 can't hold
// dropped (tab and line breaks stay).
std::string xmlText(const std::string& s) {
    std::string o;
    for (const char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') o += c;
        }
    }
    return o;
}
}  // namespace

std::string xmpPacket(const Meta& m) {
    const int rating = m.flag == Rejected ? -1 : std::clamp(m.rating, 0, 5);
    const bool label = m.label > NoLabel && m.label < kLabelCount;
    if (!rating && !label && m.title.empty() && m.caption.empty() && m.keywords.empty()) return {};
    std::string x;
    // The BOM in "begin" is how readers tell the packet's encoding (UTF-8).
    x += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
    x += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Refractory\">\n";
    x += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
    x += "  <rdf:Description rdf:about=\"\"\n";
    x += "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
    x += "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"";
    if (rating) x += "\n    xmp:Rating=\"" + std::to_string(rating) + "\"";
    if (label) x += std::string("\n    xmp:Label=\"") + labelName(m.label) + "\"";
    x += ">\n";
    auto alt = [&](const char* tag, const std::string& text) {
        if (text.empty()) return;
        x += std::string("   <") + tag + "><rdf:Alt><rdf:li xml:lang=\"x-default\">" + xmlText(text) +
             "</rdf:li></rdf:Alt></" + tag + ">\n";
    };
    alt("dc:title", m.title);
    alt("dc:description", m.caption);
    if (!m.keywords.empty()) {
        x += "   <dc:subject><rdf:Bag>\n";
        for (const std::string& k : m.keywords) x += "    <rdf:li>" + xmlText(k) + "</rdf:li>\n";
        x += "   </rdf:Bag></dc:subject>\n";
    }
    x += "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n";
    x += "<?xpacket end=\"w\"?>";
    return x;
}

nlohmann::json Meta::toJson() const {
    nlohmann::json j = {{"rating", rating}, {"flag", flag}, {"edited", edited}};
    if (!thumb.empty()) j["thumb"] = thumb;
    // Only what is set, so sidecars without metadata stay as they were.
    if (label != NoLabel) j["label"] = label;
    if (!title.empty()) j["title"] = title;
    if (!caption.empty()) j["caption"] = caption;
    if (!keywords.empty()) j["keywords"] = keywords;
    if (!stack.empty()) j["stack"] = stack;
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
    // Each on its own, so one damaged field doesn't lose the rest.
    auto str = [&](const char* key) {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    if (const auto it = j.find("label"); it != j.end() && it->is_number_integer())
        m.label = std::clamp(it->get<int>(), 0, kLabelCount - 1);
    m.title = str("title");
    m.caption = str("caption");
    m.stack = str("stack");
    if (const auto it = j.find("keywords"); it != j.end() && it->is_array())
        for (const auto& k : *it)
            if (k.is_string()) m.addKeyword(k.get<std::string>());
    return m;
}

namespace {
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

bool Meta::hasKeyword(const std::string& k) const {
    const std::string lk = lower(trim(k));
    return std::any_of(keywords.begin(), keywords.end(), [&](const std::string& e) { return lower(e) == lk; });
}

void Meta::addKeyword(const std::string& k) {
    const std::string t = trim(k);
    if (!t.empty() && !hasKeyword(t)) keywords.push_back(t);
}

void Meta::removeKeyword(const std::string& k) {
    const std::string lk = lower(trim(k));
    keywords.erase(std::remove_if(keywords.begin(), keywords.end(), [&](const std::string& e) { return lower(e) == lk; }), keywords.end());
}

std::vector<std::string> splitKeywords(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text + ",") {
        if (c != ',' && c != ';') {
            cur += c;
            continue;
        }
        if (std::string t = trim(cur); !t.empty()) out.push_back(std::move(t));
        cur.clear();
    }
    return out;
}

bool matchesSearch(const std::string& photoU8, const Meta& m, const std::string& query) {
    std::string hay = lower(pathToU8(u8ToPath(photoU8).filename())) + "\n" + lower(m.title) + "\n" + lower(m.caption);
    for (const std::string& k : m.keywords) hay += "\n" + lower(k);
    const std::string q = lower(query);
    size_t i = 0;
    while (i < q.size()) {
        const size_t a = q.find_first_not_of(' ', i);
        if (a == std::string::npos) break;
        const size_t b = std::min(q.find(' ', a), q.size());
        if (hay.find(q.substr(a, b - a)) == std::string::npos) return false;
        i = b;
    }
    return true;
}

static std::string sidecarName(const std::string& photoU8, int copy, const char* ext) {
    return copy > 0 ? photoU8 + ".copy" + std::to_string(copy) + ext : photoU8 + ext;
}

std::string sidecarPath(const std::string& photoU8, int copy) {
    const std::string side = sidecarName(photoU8, copy, kProjectExt), old = sidecarName(photoU8, copy, kLegacyProjectExt);
    std::error_code ec;
    return !fs::exists(u8ToPath(side), ec) && fs::is_regular_file(u8ToPath(old), ec) ? old : side;
}

std::string sidecarSavePath(const std::string& photoU8, int copy) {
    const std::string side = sidecarName(photoU8, copy, kProjectExt), old = sidecarName(photoU8, copy, kLegacyProjectExt);
    std::error_code ec;
    if (!fs::exists(u8ToPath(side), ec) && fs::is_regular_file(u8ToPath(old), ec)) fs::rename(u8ToPath(old), u8ToPath(side), ec);
    // If the rename failed (the file is in use), the edit still goes to the new name; the old
    // sidecar is then shadowed by it.
    return side;
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
        std::string_view ext = kProjectExt;
        if (name.size() <= ext.size() || lower(name.substr(name.size() - ext.size())) != ext) ext = kLegacyProjectExt;
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
        // A copy with both a .refract and a NodeLab .nlproj sidecar is listed once.
        it->second.erase(std::unique(it->second.begin(), it->second.end()), it->second.end());
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
    const std::string side = sidecarSavePath(photoU8, copy);
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
    const std::string side = sidecarSavePath(targetU8, targetCopy);
    if (readJson(side, old) && old.contains("ui") && old["ui"].is_object()) ui = old["ui"];
    ui["library"] = m.toJson();
    return saveProject(side, g, ui, err);
}

// ---------------------------------------------------------------- collections

namespace {
std::mutex gCollectionsMutex;
std::string gCollectionsFile;

fs::path collectionsPath() {
    std::lock_guard lock(gCollectionsMutex);
    if (!gCollectionsFile.empty()) return u8ToPath(gCollectionsFile);
    fs::path p;
#ifdef _WIN32
    if (const fs::path a = appDataDir(); !a.empty()) p = a / "collections.json";
#endif
    return p.empty() ? fs::current_path() / "collections.json" : p;
}
}  // namespace

void setCollectionsFile(const std::string& pathU8) {
    std::lock_guard lock(gCollectionsMutex);
    gCollectionsFile = pathU8;
}

std::vector<Collection> loadCollections() {
    std::vector<Collection> out;
    std::ifstream f(collectionsPath(), std::ios::binary);
    if (!f) return out;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f);
    } catch (const std::exception&) {
        return out;
    }
    const auto list = j.is_object() ? j.find("collections") : j.end();
    if (!j.is_object() || list == j.end() || !list->is_array()) return out;
    for (const auto& c : *list) {
        if (!c.is_object() || !c.contains("name") || !c["name"].is_string()) continue;
        Collection col{c["name"].get<std::string>(), {}};
        if (const auto e = c.find("photos"); e != c.end() && e->is_array())
            for (const auto& p : *e) {
                if (p.is_string()) col.entries.push_back({p.get<std::string>(), 0});
                else if (p.is_object() && p.contains("photo") && p["photo"].is_string())
                    col.entries.push_back({p["photo"].get<std::string>(), std::max(0, p.value("copy", 0))});
            }
        out.push_back(std::move(col));
    }
    return out;
}

bool saveCollections(const std::vector<Collection>& c, std::string& err) {
    nlohmann::json list = nlohmann::json::array();
    for (const Collection& col : c) {
        nlohmann::json photos = nlohmann::json::array();
        for (const Entry& e : col.entries)
            photos.push_back(e.copy > 0 ? nlohmann::json{{"photo", e.photo}, {"copy", e.copy}} : nlohmann::json(e.photo));
        list.push_back({{"name", col.name}, {"photos", photos}});
    }
    const fs::path path = collectionsPath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // writeJson wants a Refractory file; this one is plain.
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << nlohmann::json{{"collections", list}}.dump(2))) {
            err = "cannot write " + pathToU8(path);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    return true;
}

int addToCollection(std::vector<Collection>& c, const std::string& name, const std::vector<Entry>& entries) {
    auto it = std::find_if(c.begin(), c.end(), [&](const Collection& x) { return x.name == name; });
    if (it == c.end()) it = c.insert(c.end(), Collection{name, {}});
    int added = 0;
    for (const Entry& e : entries) {
        const bool there = std::any_of(it->entries.begin(), it->entries.end(),
                                       [&](const Entry& x) { return x.copy == e.copy && samePath(x.photo, e.photo); });
        if (!there) it->entries.push_back(e), ++added;
    }
    return added;
}

// ---------------------------------------------------------------- duplicates

uint64_t fileHash(const std::string& pathU8) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) return 0;
    // FNV-1a over the bytes, 64 bits at a time; the length too, so a prefix never matches.
    uint64_t h = 1469598103934665603ull, len = 0;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        const size_t n = size_t(f.gcount());
        for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(buf[i])) * 1099511628211ull;
        len += n;
    }
    h = (h ^ len) * 1099511628211ull;
    return h ? h : 1;
}

uint64_t pictureHash(const Image& img) {
    // dHash: grey at 9 x 8 (area averages), one bit per pair of neighbours (brighter or not).
    if (img.w < 1 || img.h < 1) return 0;
    float g[8][9] = {};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 9; ++x) {
            const int x0 = x * img.w / 9, x1 = std::max(x0 + 1, (x + 1) * img.w / 9);
            const int y0 = y * img.h / 8, y1 = std::max(y0 + 1, (y + 1) * img.h / 8);
            double s = 0;
            int n = 0;
            for (int v = y0; v < std::min(y1, img.h); ++v)
                for (int u = x0; u < std::min(x1, img.w); ++u, ++n) {
                    const float* p = img.pixel(size_t(v) * size_t(img.w) + size_t(u));
                    const float l = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
                    s += std::isfinite(l) ? l : 0.0f;
                }
            g[y][x] = n ? float(s / n) : 0.0f;
        }
    uint64_t h = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) h = (h << 1) | uint64_t(g[y][x + 1] > g[y][x]);
    return h;
}

std::array<uint64_t, 3> rotatedHashes(const Image& img) {
    std::array<uint64_t, 3> out{};
    if (img.w < 1 || img.h < 1) return out;
    // Turned clockwise a quarter at a time; only the grey matters, so one channel is copied.
    Image cur = img;
    for (int r = 0; r < 3; ++r) {
        Image next(cur.h, cur.w);
        for (int y = 0; y < cur.h; ++y)
            for (int x = 0; x < cur.w; ++x) {
                // (x, y) lands at column h - 1 - y, row x.
                const float* s = cur.pixel(size_t(y) * size_t(cur.w) + size_t(x));
                float* d = next.pixel(size_t(x) * size_t(next.w) + size_t(cur.h - 1 - y));
                for (int c = 0; c < 4; ++c) d[c] = s[c];
            }
        out[size_t(r)] = pictureHash(next);
        cur = std::move(next);
    }
    return out;
}

int hashDistance(uint64_t a, uint64_t b) { return __builtin_popcountll(a ^ b); }

std::vector<std::vector<int>> groupDuplicates(const std::vector<uint64_t>& files, const std::vector<uint64_t>& pictures,
                                              const std::vector<float>& aspects, int maxBits,
                                              const std::vector<std::array<uint64_t, 3>>& rotated) {
    const int n = int(files.size());
    std::vector<int> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int i) {
        while (parent[size_t(i)] != i) i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        return i;
    };
    auto aspectOf = [&](int i) { return size_t(i) < aspects.size() ? aspects[size_t(i)] : 0.0f; };
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            bool same = files[size_t(i)] && files[size_t(i)] == files[size_t(j)];
            if (!same && size_t(j) < pictures.size() && pictures[size_t(i)] && pictures[size_t(j)]) {
                const float a = aspectOf(i), b = aspectOf(j);
                // A flat picture hashes to 0 bits set or all set: only an exact file match counts.
                auto aspectOk = [](float a, float b) { return a > 0 && b > 0 && std::abs(a - b) <= 0.02f * std::max(a, b); };
                same = aspectOk(a, b) && hashDistance(pictures[size_t(i)], pictures[size_t(j)]) <= maxBits;
                // j turned by a quarter, a half or three quarters: compare i with j's turned hashes.
                if (!same && size_t(j) < rotated.size())
                    for (int r = 0; r < 3 && !same; ++r) {
                        const uint64_t h = rotated[size_t(j)][size_t(r)];
                        const float bb = r == 1 ? b : (b > 0 ? 1.0f / b : 0.0f);
                        same = h && aspectOk(a, bb) && hashDistance(pictures[size_t(i)], h) <= maxBits;
                    }
            }
            if (same) parent[size_t(find(j))] = find(i);
        }
    std::map<int, std::vector<int>> groups;
    for (int i = 0; i < n; ++i) groups[find(i)].push_back(i);
    std::vector<std::vector<int>> out;
    for (auto& [root, members] : groups)
        if (members.size() > 1) out.push_back(std::move(members));
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    return out;
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
