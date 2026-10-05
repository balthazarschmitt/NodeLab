#include "io/LensProfiles.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "io/Paths.h"
#include "ml/Http.h"

namespace fs = std::filesystem;

namespace lensdb {

// ---------------------------------------------------------------- the corrections

namespace {

float finiteOr(const nlohmann::json& j, const char* key, float def) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    const float v = it->get<float>();
    return std::isfinite(v) ? v : def;
}

void readArray(const nlohmann::json& j, const char* key, float* out, int n, float lo, float hi) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return;
    for (int i = 0; i < n && i < int(it->size()); ++i)
        if ((*it)[size_t(i)].is_number()) {
            const float v = (*it)[size_t(i)].get<float>();
            if (std::isfinite(v)) out[i] = std::clamp(v, lo, hi);
        }
}

}  // namespace

nlohmann::json Profile::toJson() const {
    nlohmann::json j = {{"lens", lens}, {"camera", camera}, {"focal", focal}, {"aperture", aperture},
                        {"distScale", distScale}, {"vigScale", vigScale}};
    if (distModel != DistNone) j["distortion"] = {{"model", distModel}, {"k", {dist[0], dist[1], dist[2]}}};
    if (tcaModel != TcaNone)
        j["tca"] = {{"model", tcaModel}, {"red", {tcaR[0], tcaR[1], tcaR[2]}}, {"blue", {tcaB[0], tcaB[1], tcaB[2]}}};
    if (vig) j["vignetting"] = {{"k", {vigK[0], vigK[1], vigK[2]}}};
    return j;
}

Profile Profile::fromJson(const nlohmann::json& j) {
    Profile p;
    if (!j.is_object()) return p;
    if (j.contains("lens") && j["lens"].is_string()) p.lens = j["lens"].get<std::string>();
    if (j.contains("camera") && j["camera"].is_string()) p.camera = j["camera"].get<std::string>();
    p.focal = std::clamp(finiteOr(j, "focal", 0), 0.0f, 10000.0f);
    p.aperture = std::clamp(finiteOr(j, "aperture", 0), 0.0f, 1000.0f);
    // Scales far outside any real camera would only produce garbage.
    p.distScale = std::clamp(finiteOr(j, "distScale", 1), 0.05f, 20.0f);
    p.vigScale = std::clamp(finiteOr(j, "vigScale", 1), 0.05f, 20.0f);
    if (auto it = j.find("distortion"); it != j.end() && it->is_object()) {
        const float m = finiteOr(*it, "model", 0);
        if (m >= Poly3 && m <= PTLens) {
            p.distModel = int(m);
            readArray(*it, "k", p.dist, 3, -2.0f, 2.0f);
        }
    }
    if (auto it = j.find("tca"); it != j.end() && it->is_object()) {
        const float m = finiteOr(*it, "model", 0);
        if (m >= TcaLinear && m <= TcaPoly3) {
            p.tcaModel = int(m);
            p.tcaR[0] = p.tcaB[0] = 1.0f;
            if (p.tcaModel == TcaPoly3) p.tcaR[0] = p.tcaB[0] = 0.0f, p.tcaR[2] = p.tcaB[2] = 1.0f;
            readArray(*it, "red", p.tcaR, 3, -2.0f, 2.0f);
            readArray(*it, "blue", p.tcaB, 3, -2.0f, 2.0f);
        }
    }
    if (auto it = j.find("vignetting"); it != j.end() && it->is_object()) {
        p.vig = true;
        readArray(*it, "k", p.vigK, 3, -5.0f, 5.0f);
    }
    return p;
}

std::string Profile::signature() const { return valid() ? toJson().dump() : std::string(); }

double distort(const Profile& p, double ru) {
    const double r2 = ru * ru;
    switch (p.distModel) {
    case Profile::Poly3: return ru * (1.0 - p.dist[0] + p.dist[0] * r2);
    case Profile::Poly5: return ru * (1.0 + p.dist[0] * r2 + p.dist[1] * r2 * r2);
    case Profile::PTLens: {
        const double a = p.dist[0], b = p.dist[1], c = p.dist[2];
        return ru * (a * r2 * ru + b * r2 + c * ru + 1.0 - a - b - c);
    }
    default: return ru;
    }
}

double tcaScale(const Profile& p, int c, double rd) {
    const float* k = c == 0 ? p.tcaR : p.tcaB;
    switch (p.tcaModel) {
    case Profile::TcaLinear: return k[0];
    case Profile::TcaPoly3: return k[0] * rd * rd + k[1] * rd + k[2];
    default: return 1.0;
    }
}

double vignetteGain(const Profile& p, double r) {
    if (!p.vig) return 1.0;
    const double r2 = r * r;
    const double v = 1.0 + p.vigK[0] * r2 + p.vigK[1] * r2 * r2 + p.vigK[2] * r2 * r2 * r2;
    return 1.0 / std::max(v, 0.05);  // a damaged profile can't divide by zero or flip the sign
}

// ---------------------------------------------------------------- a small XML reader

namespace {

struct XmlNode {
    std::string name, text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> kids;

    const std::string* attr(const char* n) const {
        for (const auto& [k, v] : attrs)
            if (k == n) return &v;
        return nullptr;
    }
};

void appendUtf8(std::string& s, unsigned cp) {
    if (cp == 0 || cp > 0x10FFFF) return;
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6)), s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12)), s += char(0x80 | ((cp >> 6) & 0x3F)), s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18)), s += char(0x80 | ((cp >> 12) & 0x3F)), s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

std::string decodeEntities(const char* b, const char* e) {
    std::string s;
    s.reserve(size_t(e - b));
    while (b < e) {
        if (*b != '&') {
            s += *b++;
            continue;
        }
        const char* semi = std::find(b, std::min(e, b + 12), ';');
        if (semi == std::min(e, b + 12)) {
            s += *b++;
            continue;
        }
        const std::string ent(b + 1, semi);
        if (ent == "amp") s += '&';
        else if (ent == "lt") s += '<';
        else if (ent == "gt") s += '>';
        else if (ent == "quot") s += '"';
        else if (ent == "apos") s += '\'';
        else if (ent.size() > 1 && ent[0] == '#')
            appendUtf8(s, unsigned(std::strtoul(ent.c_str() + (ent[1] == 'x' ? 2 : 1), nullptr, ent[1] == 'x' ? 16 : 10)));
        b = semi + 1;
    }
    return s;
}

class XmlReader {
public:
    XmlReader(const std::string& s) : p_(s.data()), e_(s.data() + s.size()) {}

    // The document's root element; false on malformed input.
    bool parse(XmlNode& root) {
        if (!skipMisc()) return false;
        return p_ < e_ && *p_ == '<' && element(root, 0);
    }

private:
    const char* p_;
    const char* e_;

    static bool nameChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ':' || c == '.';
    }
    bool starts(const char* lit) const {
        const size_t n = std::char_traits<char>::length(lit);
        return size_t(e_ - p_) >= n && std::equal(lit, lit + n, p_);
    }
    bool skipPast(const char* lit) {
        const size_t n = std::char_traits<char>::length(lit);
        const char* f = std::search(p_, e_, lit, lit + n);
        if (f == e_) return false;
        p_ = f + n;
        return true;
    }
    void skipSpace() {
        while (p_ < e_ && std::isspace(static_cast<unsigned char>(*p_))) ++p_;
    }
    // Declarations, comments and whitespace between elements.
    bool skipMisc() {
        for (;;) {
            skipSpace();
            if (starts("<?")) {
                if (!skipPast("?>")) return false;
            } else if (starts("<!--")) {
                if (!skipPast("-->")) return false;
            } else if (starts("<!")) {
                if (!skipPast(">")) return false;
            } else {
                return true;
            }
        }
    }
    std::string name() {
        const char* b = p_;
        while (p_ < e_ && nameChar(*p_)) ++p_;
        return std::string(b, p_);
    }

    bool element(XmlNode& n, int depth) {
        if (depth > 32) return false;
        ++p_;  // '<'
        n.name = name();
        if (n.name.empty()) return false;
        for (;;) {
            skipSpace();
            if (p_ >= e_) return false;
            if (*p_ == '/') {
                if (++p_ >= e_ || *p_ != '>') return false;
                ++p_;
                return true;
            }
            if (*p_ == '>') {
                ++p_;
                break;
            }
            std::string key = name();
            if (key.empty()) return false;
            skipSpace();
            if (p_ >= e_ || *p_ != '=') return false;
            ++p_;
            skipSpace();
            if (p_ >= e_ || (*p_ != '"' && *p_ != '\'')) return false;
            const char q = *p_++;
            const char* b = p_;
            while (p_ < e_ && *p_ != q) ++p_;
            if (p_ >= e_) return false;
            n.attrs.emplace_back(std::move(key), decodeEntities(b, p_));
            ++p_;
        }
        // Content: text, children, comments, CDATA, then the closing tag.
        for (;;) {
            const char* b = p_;
            while (p_ < e_ && *p_ != '<') ++p_;
            n.text += decodeEntities(b, p_);
            if (p_ >= e_) return false;
            if (starts("</")) {
                p_ += 2;
                if (name() != n.name) return false;
                skipSpace();
                if (p_ >= e_ || *p_ != '>') return false;
                ++p_;
                return true;
            }
            if (starts("<!--")) {
                if (!skipPast("-->")) return false;
            } else if (starts("<![CDATA[")) {
                p_ += 9;
                const char* cb = p_;
                if (!skipPast("]]>")) return false;
                n.text.append(cb, p_ - 3);
            } else if (starts("<?") || starts("<!")) {
                if (!skipPast(">")) return false;
            } else {
                n.kids.emplace_back();
                if (!element(n.kids.back(), depth + 1)) return false;
            }
        }
    }
};

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The element's text in the default language (lensfun adds translated <model lang="de">).
std::string childText(const XmlNode& n, const char* name) {
    for (const XmlNode& k : n.kids)
        if (k.name == name && !k.attr("lang")) return trim(k.text);
    return {};
}

float number(const std::string* s, float def) {
    if (!s) return def;
    char* end = nullptr;
    const float v = std::strtof(s->c_str(), &end);
    return end != s->c_str() && std::isfinite(v) ? v : def;
}

float textNumber(const XmlNode& n, const char* name, float def) {
    const std::string t = childText(n, name);
    return number(t.empty() ? nullptr : &t, def);
}

// "3:2", "4:3" or a plain ratio.
float aspectOf(const std::string& s, float def) {
    if (s.empty()) return def;
    float a = 0, b = 0;
    if (std::sscanf(s.c_str(), "%f:%f", &a, &b) == 2 && a > 0 && b > 0) return std::max(a, b) / std::min(a, b);
    const float v = number(&s, def);
    return v >= 1.0f && v < 10.0f ? v : def;
}

void readCalibration(const XmlNode& cal, Lens& lens) {
    // Newer databases put the sensor on <calibration>; older ones on the lens.
    const float crop = std::clamp(number(cal.attr("cropfactor"), lens.crop), 0.1f, 100.0f);
    const float aspect = cal.attr("aspect-ratio") ? aspectOf(*cal.attr("aspect-ratio"), lens.aspect) : lens.aspect;
    for (const XmlNode& m : cal.kids) {
        Calib c;
        c.crop = crop, c.aspect = aspect;
        c.focal = number(m.attr("focal"), 0);
        if (!(c.focal > 0) || c.focal > 10000) continue;
        const std::string model = m.attr("model") ? *m.attr("model") : "";
        const auto k = [&](const char* a, float def) { return std::clamp(number(m.attr(a), def), -2.0f, 2.0f); };
        if (m.name == "distortion") {
            if (model == "poly3") c.model = Profile::Poly3, c.k[0] = k("k1", 0);
            else if (model == "poly5") c.model = Profile::Poly5, c.k[0] = k("k1", 0), c.k[1] = k("k2", 0);
            else if (model == "ptlens") c.model = Profile::PTLens, c.k[0] = k("a", 0), c.k[1] = k("b", 0), c.k[2] = k("c", 0);
            else continue;
            lens.dist.push_back(c);
        } else if (m.name == "tca") {
            if (model == "linear") {
                c.model = Profile::TcaLinear, c.k[0] = k("kr", 1), c.k[3] = k("kb", 1);
            } else if (model == "poly3") {
                c.model = Profile::TcaPoly3;
                c.k[0] = k("br", 0), c.k[1] = k("cr", 0), c.k[2] = k("vr", 1);
                c.k[3] = k("bb", 0), c.k[4] = k("cb", 0), c.k[5] = k("vb", 1);
            } else {
                continue;
            }
            lens.tca.push_back(c);
        } else if (m.name == "vignetting") {
            if (model != "pa") continue;
            c.aperture = std::max(number(m.attr("aperture"), 0), 0.0f);
            c.distance = std::max(number(m.attr("distance"), 1000), 0.0f);
            c.k[0] = std::clamp(number(m.attr("k1"), 0), -5.0f, 5.0f);
            c.k[1] = std::clamp(number(m.attr("k2"), 0), -5.0f, 5.0f);
            c.k[2] = std::clamp(number(m.attr("k3"), 0), -5.0f, 5.0f);
            lens.vig.push_back(c);
        }
    }
}

// Words and numbers of a name, for matching EXIF strings against the database's: "EF24-105mm
// f/4L IS USM" gives ef 24 105 4 l is usm. Units carry nothing.
std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isdigit(c)) {
            size_t j = i;
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
            // 4.0 and 4 are the same number.
            std::string t = s.substr(i, j - i);
            if (t.find('.') != std::string::npos) {
                while (!t.empty() && t.back() == '0') t.pop_back();
                if (!t.empty() && t.back() == '.') t.pop_back();
            }
            if (!t.empty()) out.push_back(t);
            i = j;
        } else if (std::isalpha(c)) {
            size_t j = i;
            while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
            std::string t = lower(s.substr(i, j - i));
            if (t != "mm" && t != "f") out.push_back(t);
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

bool isNumber(const std::string& t) { return !t.empty() && std::isdigit(static_cast<unsigned char>(t[0])); }

bool sameMaker(const std::string& a, const std::string& b) {
    // "NIKON CORPORATION" and "Nikon": compare the first words.
    const auto ta = tokens(a), tb = tokens(b);
    return !ta.empty() && !tb.empty() && ta[0] == tb[0];
}

}  // namespace

bool Database::loadXml(const std::string& xml) {
    XmlNode root;
    if (!XmlReader(xml).parse(root) || root.name != "lensdatabase") return false;
    for (const XmlNode& n : root.kids) {
        if (n.name == "camera") {
            Camera c;
            c.maker = childText(n, "maker"), c.model = childText(n, "model"), c.mount = childText(n, "mount");
            c.crop = std::clamp(textNumber(n, "cropfactor", 1), 0.1f, 100.0f);
            if (!c.model.empty()) cameras_.push_back(std::move(c));
        } else if (n.name == "lens") {
            Lens l;
            l.maker = childText(n, "maker"), l.model = childText(n, "model");
            if (l.model.empty()) continue;
            for (const XmlNode& k : n.kids) {
                if (k.name == "mount") l.mounts.push_back(trim(k.text));
                if (k.name == "type") l.fisheye = trim(k.text).find("fisheye") != std::string::npos;
            }
            l.crop = std::clamp(textNumber(n, "cropfactor", 1), 0.1f, 100.0f);
            l.aspect = aspectOf(childText(n, "aspect-ratio"), 1.5f);
            for (const XmlNode& k : n.kids)
                if (k.name == "calibration") readCalibration(k, l);
            const auto byFocal = [](const Calib& a, const Calib& b) { return a.focal < b.focal; };
            std::stable_sort(l.dist.begin(), l.dist.end(), byFocal);
            std::stable_sort(l.tca.begin(), l.tca.end(), byFocal);
            if (!l.dist.empty() || !l.tca.empty() || !l.vig.empty()) lenses_.push_back(std::move(l));
        }
    }
    return true;
}

int Database::loadFolder(const std::string& dirU8) {
    int n = 0;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(u8ToPath(dirU8), ec))
        if (e.path().extension() == ".xml") files.push_back(e.path());
    std::sort(files.begin(), files.end());  // the same order, so ties pick the same lens
    for (const fs::path& f : files) {
        std::vector<char> bytes;
        if (readFileBytes(pathToU8(f), bytes) && loadXml(std::string(bytes.begin(), bytes.end()))) ++n;
    }
    return n;
}

const Camera* Database::findCamera(const std::string& make, const std::string& model) const {
    const std::string m = lower(trim(model));
    if (m.empty()) return nullptr;
    const Camera* best = nullptr;
    for (const Camera& c : cameras_) {
        if (!make.empty() && !sameMaker(make, c.maker)) continue;
        if (lower(c.model) == m) return &c;
        // EXIF models sometimes repeat or drop the maker ("Canon EOS 80D" or "EOS 80D").
        if (!best && tokens(c.model) == tokens(model)) best = &c;
        if (!best) {
            auto a = tokens(c.model), b = tokens(model);
            const auto mk = tokens(c.maker);
            if (!mk.empty() && !a.empty() && a[0] == mk[0]) a.erase(a.begin());
            if (!mk.empty() && !b.empty() && b[0] == mk[0]) b.erase(b.begin());
            if (!a.empty() && a == b) best = &c;
        }
    }
    return best;
}

std::vector<Match> Database::findLenses(const std::string& lensName, const Camera* camera) const {
    std::vector<Match> out;
    const auto mountOk = [&](const Lens& l) {
        return camera && std::find(l.mounts.begin(), l.mounts.end(), camera->mount) != l.mounts.end();
    };
    if (trim(lensName).empty()) {
        // Compact cameras: their lens has the camera's own mount.
        if (camera)
            for (const Lens& l : lenses_)
                if (mountOk(l)) out.push_back({&l, 1.0});
        return out;
    }
    const std::vector<std::string> e = tokens(lensName);
    const std::set<std::string> es(e.begin(), e.end());
    for (const Lens& l : lenses_) {
        std::vector<std::string> d = tokens(l.model);
        const std::vector<std::string> mk = tokens(l.maker);
        // "Canon EF 24-105mm": the maker needn't be in the EXIF name.
        if (!mk.empty() && !d.empty() && d[0] == mk[0] && !es.count(d[0])) d.erase(d.begin());
        const std::set<std::string> ds(d.begin(), d.end());
        // The database name's focal lengths and apertures must all be in the EXIF name: 24-70 f/4
        // isn't 24-105 f/4. EXIF names add numbers of their own (Sigma's "| Art 013"), which
        // only count against a match.
        bool numbersAgree = true;
        int numbers = 0, extra = 0;
        for (const std::string& t : ds)
            if (isNumber(t)) ++numbers, numbersAgree &= es.count(t) > 0;
        for (const std::string& t : es)
            if (isNumber(t) && !ds.count(t)) ++extra;
        if (!numbersAgree || numbers == 0) continue;
        int common = 0;
        for (const std::string& t : ds) common += int(es.count(t));
        std::set<std::string> all = ds;
        all.insert(es.begin(), es.end());
        double score = double(common) / double(all.size()) - 0.1 * extra;
        if (mountOk(l)) score += 0.3;
        if (camera && sameMaker(camera->maker, l.maker)) score += 0.1;
        out.push_back({&l, score});
    }
    std::stable_sort(out.begin(), out.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    return out;
}

// ---------------------------------------------------------------- interpolation

namespace {

// Linear in focal length between the neighbouring calibrations of the same model (lensfun
// uses a spline; between the close steps a profile has, the two agree well).
const Calib* focalPair(const std::vector<Calib>& v, float focal, const Calib*& hi, float& t) {
    hi = nullptr, t = 0;
    if (v.empty()) return nullptr;
    if (focal <= v.front().focal) return &v.front();
    if (focal >= v.back().focal) return &v.back();
    for (size_t i = 0; i + 1 < v.size(); ++i)
        if (focal >= v[i].focal && focal <= v[i + 1].focal) {
            if (v[i].model != v[i + 1].model || v[i + 1].focal <= v[i].focal) {
                // Different models can't be blended: the nearer one.
                return focal - v[i].focal <= v[i + 1].focal - focal ? &v[i] : &v[i + 1];
            }
            hi = &v[i + 1];
            t = (focal - v[i].focal) / (v[i + 1].focal - v[i].focal);
            return &v[i];
        }
    return &v.back();
}

void blend(const Calib* lo, const Calib* hi, float t, float* out, int n, int offset = 0) {
    for (int i = 0; i < n; ++i) out[i] = hi ? lo->k[offset + i] * (1 - t) + hi->k[offset + i] * t : lo->k[offset + i];
}

}  // namespace

Profile resolve(const Lens& lens, const Camera* camera, float focal, float aperture) {
    Profile p;
    p.lens = lens.maker.empty() || lens.model.rfind(lens.maker, 0) == 0 ? lens.model : lens.maker + " " + lens.model;
    p.camera = camera ? camera->model : std::string();
    // Unknown focal length: the shortest (fixed-lens compacts have one).
    float minF = 1e9f;
    for (const auto* v : {&lens.dist, &lens.tca, &lens.vig})
        for (const Calib& c : *v) minF = std::min(minF, c.focal);
    p.focal = focal > 0 ? focal : (minF < 1e9f ? minF : 0);
    p.aperture = aperture;
    const float imgCrop = camera ? camera->crop : lens.crop;

    const Calib* hi = nullptr;
    float t = 0;
    float calCrop = lens.crop, calAspect = lens.aspect;
    if (const Calib* lo = focalPair(lens.dist, p.focal, hi, t)) {
        p.distModel = lo->model;
        blend(lo, hi, t, p.dist, 3);
        calCrop = lo->crop, calAspect = lo->aspect;
    }
    if (const Calib* lo = focalPair(lens.tca, p.focal, hi, t)) {
        p.tcaModel = lo->model;
        blend(lo, hi, t, p.tcaR, 3);
        blend(lo, hi, t, p.tcaB, 3, 3);
        if (p.distModel == Profile::DistNone) calCrop = lo->crop, calAspect = lo->aspect;
    }
    p.distScale = float(std::sqrt(double(calAspect) * calAspect + 1.0) * calCrop / imgCrop);

    if (!lens.vig.empty()) {
        // Focused far away: the farthest distance measured for each focal length and aperture.
        std::map<std::pair<float, float>, const Calib*> far;
        for (const Calib& c : lens.vig) {
            const Calib*& f = far[{c.focal, c.aperture}];
            if (!f || c.distance > f->distance) f = &c;
        }
        float fLo = 1e9f, fHi = 0, aLo = 1e9f, aHi = 0;
        for (const auto& [key, c] : far) {
            fLo = std::min(fLo, c->focal), fHi = std::max(fHi, c->focal);
            if (c->aperture > 0) aLo = std::min(aLo, std::log2(c->aperture)), aHi = std::max(aHi, std::log2(c->aperture));
        }
        // Unknown aperture: f/8, or as near as measured.
        const float a = std::log2(aperture > 0 ? aperture : 8.0f);
        // Inverse distance weighting over focal length and stops, each spread over its range.
        const float fSpan = std::max(fHi - fLo, 1.0f), aSpan = std::max(aHi - aLo, 1.0f);
        double wsum = 0, k[3] = {};
        const Calib* exact = nullptr;
        for (const auto& [key, c] : far) {
            const double df = (c->focal - p.focal) / fSpan;
            const double da = c->aperture > 0 ? (std::log2(c->aperture) - a) / aSpan : 0.0;
            const double d2 = df * df + da * da;
            if (d2 < 1e-12) {
                exact = c;
                break;
            }
            const double w = 1.0 / (d2 * d2);
            wsum += w;
            for (int i = 0; i < 3; ++i) k[i] += w * c->k[i];
        }
        p.vig = true;
        for (int i = 0; i < 3; ++i) p.vigK[i] = exact ? exact->k[i] : float(k[i] / wsum);
        const Calib& any = exact ? *exact : *far.begin()->second;
        p.vigScale = any.crop / imgCrop;
    }
    return p;
}

// ---------------------------------------------------------------- the downloaded database

namespace {

// lensfun's database at a release tag, so a download always gets the same files.
constexpr const char* kBaseUrl = "https://raw.githubusercontent.com/lensfun/lensfun/v0.3.4/data/db/";
const char* const kFiles[] = {
    "6x6.xml", "actioncams.xml", "compact-canon.xml", "compact-casio.xml", "compact-fujifilm.xml",
    "compact-kodak.xml", "compact-konica-minolta.xml", "compact-leica.xml", "compact-nikon.xml",
    "compact-olympus.xml", "compact-panasonic.xml", "compact-pentax.xml", "compact-ricoh.xml",
    "compact-samsung.xml", "compact-sigma.xml", "compact-sony.xml", "contax.xml", "generic.xml",
    "mil-canon.xml", "mil-fujifilm.xml", "mil-leica.xml", "mil-nikon.xml", "mil-olympus.xml",
    "mil-panasonic.xml", "mil-pentax.xml", "mil-samsung.xml", "mil-samyang.xml", "mil-sigma.xml",
    "mil-sony.xml", "mil-tamron.xml", "mil-tokina.xml", "mil-zeiss.xml", "misc.xml", "om-system.xml",
    "rf-leica.xml", "slr-canon.xml", "slr-hasselblad.xml", "slr-konica-minolta.xml", "slr-leica.xml",
    "slr-nikon.xml", "slr-olympus.xml", "slr-panasonic.xml", "slr-pentax.xml", "slr-ricoh.xml",
    "slr-samsung.xml", "slr-samyang.xml", "slr-schneider.xml", "slr-sigma.xml", "slr-soligor.xml",
    "slr-sony.xml", "slr-tamron.xml", "slr-tokina.xml", "slr-ussr.xml", "slr-vivitar.xml", "slr-zeiss.xml"};

std::mutex g_m;
std::string g_override;
std::shared_ptr<const Database> g_db;
std::string g_dbFolder;
DownloadState g_state;
std::atomic<bool> g_cancel{false};

}  // namespace

std::string folder() {
    fs::path p;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_override.empty()) p = u8ToPath(g_override);
    }
    if (p.empty()) {
#ifdef _WIN32
        // UI scripts point it at a made-up database (tests/ui/lensfun).
        if (const wchar_t* dir = _wgetenv(L"NODELAB_LENSFUN_DIR")) p = fs::path(dir);
        else if (const wchar_t* appdata = _wgetenv(L"APPDATA")) p = fs::path(appdata) / "NodeLab" / "lensfun";
#endif
        if (p.empty()) p = fs::current_path() / "lensfun";
    }
    std::error_code ec;
    fs::create_directories(p, ec);
    return pathToU8(p);
}

void setFolder(const std::string& dirU8) {
    std::lock_guard<std::mutex> lock(g_m);
    g_override = dirU8;
    g_db.reset();
}

bool installed() {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(u8ToPath(folder()), ec))
        if (e.path().extension() == ".xml") return true;
    return false;
}

std::shared_ptr<const Database> shared() {
    const std::string dir = folder();
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_db || g_dbFolder != dir) {
        auto db = std::make_shared<Database>();
        db->loadFolder(dir);
        g_db = db, g_dbFolder = dir;
    }
    return g_db;
}

void startDownload() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_state.running) return;
        g_state = DownloadState{true, 0, int(std::size(kFiles)), {}};
    }
    g_cancel = false;
    std::thread([] {
        const fs::path dir = u8ToPath(folder());
        std::string err;
        for (const char* name : kFiles) {
            if (g_cancel) {
                err = "Cancelled";
                break;
            }
            std::string body;
            err = http::get(std::string(kBaseUrl) + name, {}, [&](const char* d, size_t n) {
                body.append(d, n);
                return !g_cancel && body.size() < (64u << 20);
            });
            if (err.empty() && body.find("<lensdatabase") == std::string::npos) err = std::string(name) + " isn't a lens database";
            if (!err.empty()) break;
            // Written aside and renamed, so a half-written file is never read.
            const fs::path part = dir / (std::string(name) + ".part");
            {
                std::ofstream f(part, std::ios::binary);
                f.write(body.data(), std::streamsize(body.size()));
                if (!f) err = "Couldn't write " + pathToU8(part);
            }
            std::error_code ec;
            if (err.empty()) fs::rename(part, dir / name, ec);
            if (ec) err = ec.message();
            if (!err.empty()) break;
            std::lock_guard<std::mutex> lock(g_m);
            ++g_state.done;
        }
        std::lock_guard<std::mutex> lock(g_m);
        g_state.running = false;
        g_state.error = err;
        g_db.reset();  // read the new files on next use
    }).detach();
}

void cancelDownload() { g_cancel = true; }

DownloadState downloadState() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_state;
}

}  // namespace lensdb
