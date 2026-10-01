#include "gpu/PointOp.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "gpu/Device.h"
#include "gpu/GL.h"
#include "nodes/NodeUtil.h"

namespace gpu {

const char* const kGlslCommon = R"GLSL(
float luminance(vec3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
float clamp01(float v) { return clamp(v, 0.0, 1.0); }
vec3 clamp01(vec3 v) { return clamp(v, 0.0, 1.0); }
// Colour nodes: 0..1 in legacy projects, only negatives removed in scene-linear ones.
float clampColor(bool lin, float v) { return lin ? max(v, 0.0) : clamp(v, 0.0, 1.0); }
vec3 clampColor(bool lin, vec3 v) { return lin ? max(v, 0.0) : clamp(v, 0.0, 1.0); }
// NaN and infinities become 0, by their bits (compilers may assume floats are finite).
float finiteOr0(float v) { return (floatBitsToUint(v) & 0x7F800000u) == 0x7F800000u ? 0.0 : v; }
const float kNaN = uintBitsToFloat(0x7FC00000u);
const float kInfinity = uintBitsToFloat(0x7F800000u);
// pow for x >= 0 with C's results at 0 (GLSL leaves pow(0, y) undefined).
float powPos(float x, float y) { return x <= 0.0 ? (y == 0.0 ? 1.0 : (y > 0.0 ? 0.0 : kInfinity)) : pow(x, y); }
vec3 powPos(vec3 x, vec3 y) { return vec3(powPos(x.r, y.r), powPos(x.g, y.g), powPos(x.b, y.b)); }
float srgbToLinear(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float linearToSrgb(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * powPos(max(c, 0.0), 1.0 / 2.4) - 0.055; }
vec3 srgbToLinear(vec3 c) { return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)); }
vec3 linearToSrgb(vec3 c) { return vec3(linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b)); }
vec3 rgbToHsv(vec3 c) {
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float s = mx > 1e-6 ? d / mx : 0.0;
    if (d < 1e-6) return vec3(0.0, s, mx);
    float h;
    if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
    else h = (c.r - c.g) / d + 4.0;
    return vec3(h / 6.0, s, mx);
}
vec3 hsvToRgb(vec3 hsv) {
    float h = hsv.x - floor(hsv.x), s = hsv.y, v = hsv.z;
    float f = h * 6.0;
    int i = int(f) % 6;
    f -= floor(f);
    float p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
    if (i == 0) return vec3(v, t, p);
    if (i == 1) return vec3(q, v, p);
    if (i == 2) return vec3(p, v, t);
    if (i == 3) return vec3(p, q, v);
    if (i == 4) return vec3(t, p, v);
    return vec3(v, p, q);
}
vec3 rgbToHsl(vec3 c) {
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float l = (mx + mn) * 0.5, d = mx - mn;
    if (d < 1e-6) return vec3(0.0, 0.0, l);
    float s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    return vec3(rgbToHsv(c).x, s, l);
}
vec3 hslToRgb(vec3 hsl) {
    float c = (1.0 - abs(2.0 * hsl.z - 1.0)) * hsl.y;
    float v = hsl.z + c * 0.5;
    return hsvToRgb(vec3(hsl.x, v > 1e-6 ? c / v : 0.0, v));
}
float labF(float t) { return t > 0.008856 ? pow(t, 1.0 / 3.0) : 7.787 * t + 16.0 / 116.0; }
float labFinv(float t) { return t * t * t > 0.008856 ? t * t * t : (t - 16.0 / 116.0) / 7.787; }
vec3 rgbToLab(vec3 c, bool encoded) {
    vec3 l = encoded ? srgbToLinear(c) : c;
    float x = (0.4124564 * l.r + 0.3575761 * l.g + 0.1804375 * l.b) / 0.95047;
    float y = 0.2126729 * l.r + 0.7151522 * l.g + 0.0721750 * l.b;
    float z = (0.0193339 * l.r + 0.1191920 * l.g + 0.9503041 * l.b) / 1.08883;
    float fx = labF(x), fy = labF(y), fz = labF(z);
    return vec3((116.0 * fy - 16.0) / 100.0, 500.0 * (fx - fy) / 128.0, 200.0 * (fy - fz) / 128.0);
}
vec3 labToRgb(vec3 lab, bool encoded) {
    float fy = (lab.x * 100.0 + 16.0) / 116.0;
    float fx = fy + lab.y * 128.0 / 500.0, fz = fy - lab.z * 128.0 / 200.0;
    float x = labFinv(fx) * 0.95047, y = labFinv(fy), z = labFinv(fz) * 1.08883;
    vec3 l = vec3(3.2404542 * x - 1.5371385 * y - 0.4985314 * z, -0.9692660 * x + 1.8760108 * y + 0.0415560 * z,
                  0.0556434 * x - 0.2040259 * y + 1.0572252 * z);
    return encoded ? linearToSrgb(l) : l;
}
vec3 rgbToYCbCr(vec3 c) {
    float y = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    return vec3(y, (c.b - y) / 1.8556 + 0.5, (c.r - y) / 1.5748 + 0.5);
}
vec3 yCbCrToRgb(vec3 v) {
    float cb = v.y - 0.5, cr = v.z - 0.5;
    float r = v.x + 1.5748 * cr, b = v.x + 1.8556 * cb;
    return vec3(r, (v.x - 0.2126 * r - 0.0722 * b) / 0.7152, b);
}
vec3 rgbToYuv(vec3 c) {
    float y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    return vec3(y, 0.492 * (c.b - y), 0.877 * (c.r - y));
}
vec3 yuvToRgb(vec3 v) { return vec3(v.x + 1.140 * v.z, v.x - 0.395 * v.y - 0.581 * v.z, v.x + 2.032 * v.y); }
float cbrtC(float x) { return x == 0.0 ? 0.0 : sign(x) * pow(abs(x), 1.0 / 3.0); }
vec3 rgbToOklab(vec3 c) {
    float l = cbrtC(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
    float m = cbrtC(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    float s = cbrtC(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    return vec3(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
}
vec3 oklabToRgb(vec3 lab) {
    float l = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    l = l * l * l, m = m * m * m, s = s * s * s;
    return vec3(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}
// C's atan2, which is 0 at the origin (GLSL leaves atan(0, 0) undefined).
float atan2C(float y, float x) { return x == 0.0 && y == 0.0 ? 0.0 : atan(y, x); }
vec3 compressToGamut(vec3 c) {
    float lo = min(c.r, min(c.g, c.b));
    if (lo >= 0.0) return c;
    float y = max(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b, 0.0);
    if (y <= 0.0) return vec3(0.0);
    float t = y / (y - lo);
    return max(y + t * (c - y), 0.0);
}
)GLSL";

bool gpuInput(const Value&) {
    return true;  // every kind of value has a GPU form (sized ones are uploaded)
}

bool sizedValue(const Value& v) {
    int w, h;
    return v.size(w, h);
}

namespace {

enum class Kind { None, Constant, ImageTex, ChannelTex, Fused };

std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", double(v));
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // GLSL float literal
    return s;
}

}  // namespace

// One node's point op with what feeds each input: a constant, a texture, or another pending
// stage that runs inline (fusion). Stages are immutable once made, so several shaders can share one.
struct Stage {
    struct Input {
        Kind kind = Kind::None;
        float constant = 0.0f;
        Value src;  // the GPU value read (textures and fused stages), kept alive with the stage
        const Texture* tex = nullptr;
        int tw = 0, th = 0;
        std::shared_ptr<const Stage> fused;  // Kind::Fused: the producer, and which of its outputs
        int fusedOut = 0;
        float lo = -1e30f, hi = 1e30f;  // paramSampler's range (par<i>)
    };
    PointOp op;
    std::vector<Input> inputs;
    std::vector<bool> outImage;  // per output: Image (vec4) or Channel (float)
};

// A point op not run yet: the stage tree rooted at the node's own stage, and the frame it runs in
// (the same for every stage, as only same-sized stages fuse).
struct Pending {
    std::shared_ptr<const Stage> root;
    nodeutil::PixelFrame frame;
    bool linear = false, half = false;
    std::vector<TexturePtr> results;  // once run
    void run();
};

namespace {

// ---- Fused shaders
//
// Each stage becomes a function sK_eval(p, inout outs...). Its body and helper functions are
// compiled as written: #defines map the names they use (P, img0, has1, METHOD, sstep...) to the
// stage's own (sK_P...) and #undefs drop them after it, so stages of the same node type, or
// sharing helper names, don't collide. An input fed by another stage calls that stage's eval.

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The text without comments.
std::string stripComments(const std::string& t) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '/' && i + 1 < t.size() && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            out += '\n';
        } else if (t[i] == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) ++i;
            ++i;
            out += ' ';
        } else {
            out += t[i];
        }
    }
    return out;
}

std::vector<std::string> identifiers(const std::string& t) {
    std::vector<std::string> ids;
    for (size_t i = 0; i < t.size();) {
        if (identStart(t[i]) && (i == 0 || !identChar(t[i - 1]))) {
            size_t j = i;
            while (j < t.size() && identChar(t[j])) ++j;
            ids.push_back(t.substr(i, j - i));
            i = j;
        } else {
            ++i;
        }
    }
    return ids;
}

// Names declared at the top level of `functions`: functions, and constants (`const int A = 1, B = 2;`).
std::vector<std::string> declaredNames(const std::string& functions) {
    const std::string t = stripComments(functions);
    std::vector<std::string> names;
    auto add = [&](const std::string& n) {
        if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    // The identifier ending just before position `end`.
    auto identBefore = [&](const std::string& st, size_t end) {
        size_t j = end;
        while (j > 0 && std::isspace(static_cast<unsigned char>(st[j - 1]))) --j;
        size_t i = j;
        while (i > 0 && identChar(st[i - 1])) --i;
        return st.substr(i, j - i);
    };
    std::string stmt;
    int parens = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '(' || c == '[') ++parens;
        if (c == ')' || c == ']') --parens;
        if (c == '{' && parens == 0) {
            // A function definition: its name precedes the parameter list. Skip the body.
            const size_t open = stmt.find('(');
            if (open != std::string::npos) add(identBefore(stmt, open));
            int depth = 1;
            while (++i < t.size() && depth > 0) depth += t[i] == '{' ? 1 : t[i] == '}' ? -1 : 0;
            --i;
            stmt.clear();
        } else if (c == ';' && parens == 0) {
            const size_t open = stmt.find('('), eq = stmt.find('=');
            if (open != std::string::npos && (eq == std::string::npos || open < eq)) {
                add(identBefore(stmt, open));  // a prototype
            } else {
                // Declarators split at top-level commas; each name ends before '=' or '['.
                int depth = 0;
                size_t from = 0;
                for (size_t k = 0; k <= stmt.size(); ++k) {
                    const char d = k < stmt.size() ? stmt[k] : ',';
                    if (d == '(' || d == '[') ++depth;
                    if (d == ')' || d == ']') --depth;
                    if (d == ',' && depth == 0) {
                        const std::string part = stmt.substr(from, k - from);
                        size_t end = part.find_first_of("=[");
                        if (end == std::string::npos) end = part.size();
                        add(identBefore(part, end));
                        from = k + 1;
                    }
                }
            }
            stmt.clear();
        } else {
            stmt += c;
        }
    }
    return names;
}

// Whether `functions` reads a stage's own inputs or params, so it can't be shared between stages.
bool stageSpecific(const std::string& functions) {
    for (const std::string& id : identifiers(stripComments(functions))) {
        if (id == "P" || id == "lutLookup" || id == "lutAt") return true;
        for (const char* pre : {"img", "ch", "par", "has", "fetch", "fetchCh", "bilinear", "size"}) {
            const size_t n = std::strlen(pre);
            if (id.size() > n && id.compare(0, n, pre) == 0 &&
                std::all_of(id.begin() + std::ptrdiff_t(n), id.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                return true;
        }
    }
    return false;
}

struct Generator {
    std::string code;
    // Everything to set before the dispatch, in the order the code declares it.
    std::vector<std::pair<std::string, std::vector<float>>> floats;
    std::vector<std::pair<std::string, std::pair<int, int>>> ivec2s;
    std::vector<const Texture*> textures;  // by texture unit
    std::vector<float> lut;
    std::unordered_map<const Stage*, int> done;
    std::unordered_map<std::string, std::string> sharedFunctions;  // text -> prefix
    int stages = 0;

    static std::string outDecls(const Stage& s) {
        std::string d;
        for (size_t k = 0; k < s.outImage.size(); ++k)
            d += std::string(s.outImage[k] ? "vec4 o" : "float o") + std::to_string(k) +
                 (s.outImage[k] ? " = vec4(0.0, 0.0, 0.0, 1.0);" : " = 0.0;");
        return d;
    }
    static std::string outArgs(const Stage& s) {
        std::string a;
        for (size_t k = 0; k < s.outImage.size(); ++k) a += ", o" + std::to_string(k);
        return a;
    }

    // Emits s (after the stages feeding it) and returns its index.
    int emit(const Stage& s) {
        if (auto it = done.find(&s); it != done.end()) return it->second;
        std::vector<int> producers(s.inputs.size(), -1);
        for (size_t i = 0; i < s.inputs.size(); ++i)
            if (s.inputs[i].kind == Kind::Fused) producers[i] = emit(*s.inputs[i].fused);
        const int idx = stages++;
        const std::string pre = "s" + std::to_string(idx) + "_";
        code += "\n// stage " + std::to_string(idx) + "\n";
        code += "uniform float " + pre + "P[" + std::to_string(std::max<size_t>(1, s.op.params.size())) + "];\n";
        if (!s.op.params.empty()) floats.push_back({pre + "P", s.op.params});
        const size_t lutBase = lut.size();
        lut.insert(lut.end(), s.op.lut.begin(), s.op.lut.end());

        std::vector<std::string> macros;  // names #defined for this stage
        auto define = [&](const std::string& name, const std::string& to) {
            code += "#define " + name + " " + to + "\n";
            macros.push_back(name);
        };
        std::string accessors;
        for (size_t i = 0; i < s.inputs.size(); ++i) {
            const Stage::Input& in = s.inputs[i];
            const std::string n = std::to_string(i), q = pre + n;  // e.g. s2_0
            std::string img, ch;
            switch (in.kind) {
                case Kind::None:
                    img = "vec4(0.0)";
                    ch = "0.0";
                    break;
                case Kind::Constant:
                    accessors += "uniform float " + q + "C;\n";
                    floats.push_back({q + "C", {in.constant}});
                    img = "vec4(vec3(" + q + "C), 1.0)";
                    ch = q + "C";
                    break;
                case Kind::ImageTex:
                case Kind::ChannelTex: {
                    const int unit = int(textures.size());
                    textures.push_back(in.tex);
                    accessors += "layout(binding = " + std::to_string(unit) + ") uniform sampler2D " + q + "T;\nuniform ivec2 " + q +
                                 "Sz;\n";
                    ivec2s.push_back({q + "Sz", {in.tw, in.th}});
                    // Nearest sample of a mismatched size, with the CPU's integer maths.
                    accessors += "ivec2 " + q + "at(ivec2 p) { return " + q + "Sz == uSize ? p : min(" + q + "Sz - 1, p * " + q +
                                 "Sz / max(uSize, ivec2(1))); }\n";
                    const std::string fetch = "texelFetch(" + q + "T, " + q + "at(p), 0)";
                    if (in.kind == Kind::ImageTex) {
                        img = fetch;
                        ch = "luminance(" + fetch + ".rgb)";
                    } else {
                        img = "vec4(vec3(" + fetch + ".r), 1.0)";
                        ch = fetch + ".r";
                    }
                    break;
                }
                case Kind::Fused: {
                    // The producer's output at p, converted as a texture of it would read.
                    const Stage& f = *in.fused;
                    const std::string call = outDecls(f) + " s" + std::to_string(producers[i]) + "_eval(p" + outArgs(f) + ");";
                    const std::string o = "o" + std::to_string(in.fusedOut);
                    const bool image = f.outImage[size_t(in.fusedOut)];
                    accessors += "vec4 " + q + "img(ivec2 p) { " + call + " return " +
                                 (image ? o : "vec4(vec3(" + o + "), 1.0)") + "; }\n";
                    accessors += "float " + q + "ch(ivec2 p) { " + call + " return " + (image ? "luminance(" + o + ".rgb)" : o) + "; }\n";
                    break;
                }
            }
            if (in.kind != Kind::Fused) {
                accessors += "vec4 " + q + "img(ivec2 p) { return " + img + "; }\n";
                accessors += "float " + q + "ch(ivec2 p) { return " + ch + "; }\n";
            }
            accessors += "const bool " + q + "has = " + (in.kind == Kind::None ? "false" : "true") + ";\n";
            if (std::find(s.op.gather.begin(), s.op.gather.end(), int(i)) != s.op.gather.end()) {
                // Reads anywhere in the pin's buffer: texels clamped to its edges, and the CPU's
                // bilinear sampling (pixel centres at +0.5) on top of them.
                const bool tex = in.kind == Kind::ImageTex || in.kind == Kind::ChannelTex;
                accessors += "const ivec2 " + q + "size = " +
                             (tex ? "ivec2(" + std::to_string(in.tw) + ", " + std::to_string(in.th) + ")" : std::string("uSize")) + ";\n";
                std::string at;
                if (in.kind == Kind::ImageTex) at = "texelFetch(" + q + "T, clamp(c, ivec2(0), " + q + "size - 1), 0)";
                else if (in.kind == Kind::ChannelTex) at = "vec4(vec3(texelFetch(" + q + "T, clamp(c, ivec2(0), " + q + "size - 1), 0).r), 1.0)";
                else at = img;
                accessors += "vec4 " + q + "fetch(ivec2 c) { return " + at + "; }\n";
                accessors += "float " + q + "fetchCh(ivec2 c) { return " +
                             (in.kind == Kind::ImageTex ? "luminance(" + q + "fetch(c).rgb)" : q + "fetch(c).r") + "; }\n";
                accessors += "vec4 " + q + "bilinear(vec2 xy, bool transparent) {\n"
                             "    if (transparent && (xy.x < 0.0 || xy.y < 0.0 || xy.x > float(" + q + "size.x) || xy.y > float(" + q +
                             "size.y))) return vec4(0.0);\n"
                             "    xy -= 0.5;\n    ivec2 i0 = ivec2(floor(xy));\n    vec2 f = xy - vec2(i0);\n"
                             "    vec4 a = " + q + "fetch(i0), b = " + q + "fetch(i0 + ivec2(1, 0)), c = " + q + "fetch(i0 + ivec2(0, 1)), d = " + q +
                             "fetch(i0 + ivec2(1, 1));\n"
                             "    vec4 top = a + (b - a) * f.x, bot = c + (d - c) * f.x;\n    return top + (bot - top) * f.y;\n}\n";
            }
            accessors += "float " + q + "par(ivec2 p) { return clamp(" + q + "ch(p), " + num(in.lo) + ", " + num(in.hi) + "); }\n";
        }
        code += accessors;

        // Helper functions: shared between stages with the same text unless they read the stage's
        // own inputs (then they're compiled inside its #defines).
        const std::vector<std::string> names = declaredNames(s.op.functions);
        const bool own = stageSpecific(s.op.functions);
        std::string fpre = pre;
        if (!own && !s.op.functions.empty()) {
            auto it = sharedFunctions.find(s.op.functions);
            if (it == sharedFunctions.end()) {
                const std::string sp = "f" + std::to_string(sharedFunctions.size()) + "_";
                it = sharedFunctions.emplace(s.op.functions, sp).first;
                for (const std::string& nm : names) code += "#define " + nm + " " + sp + nm + "\n";
                code += s.op.functions + "\n";
                for (const std::string& nm : names) code += "#undef " + nm + "\n";
            }
            fpre = it->second;
        }
        define("P", pre + "P");
        for (size_t i = 0; i < s.inputs.size(); ++i) {
            const std::string n = std::to_string(i), q = pre + n;
            define("img" + n, q + "img");
            define("ch" + n, q + "ch");
            define("par" + n, q + "par");
            define("has" + n, q + "has");
            if (std::find(s.op.gather.begin(), s.op.gather.end(), int(i)) != s.op.gather.end())
                for (const char* g : {"fetch", "fetchCh", "bilinear", "size"}) define(g + n, q + g);
        }
        if (!s.op.lut.empty()) {
            code += "#define lutLookup(off, n, x) lutLookupBase(" + std::to_string(lutBase) + " + (off), n, x)\n";
            code += "#define lutAt(i) L[" + std::to_string(lutBase) + " + (i)]\n";
            macros.push_back("lutLookup");
            macros.push_back("lutAt");
        }
        for (const std::string& nm : names) define(nm, fpre + nm);
        if (own) code += s.op.functions + "\n";
        std::string params;
        for (size_t k = 0; k < s.outImage.size(); ++k)
            params += std::string(", inout ") + (s.outImage[k] ? "vec4" : "float") + " out" + std::to_string(k);
        code += "void " + pre + "eval(ivec2 p" + params + ") {\n" + s.op.body + "\n}\n";
        for (const std::string& m : macros) code += "#undef " + m + "\n";
        done[&s] = idx;
        return idx;
    }
};

std::atomic<int> g_fused{0};

Format outFormat(bool image, bool half) { return image ? (half ? Format::RGBA16F : Format::RGBA32F) : Format::R32F; }

// Compiles and runs the tree rooted at `root` into new textures.
std::vector<TexturePtr> dispatchTree(const Stage& root, const Pending& pd) {
    const int w = root.op.w, h = root.op.h;
    Generator g;
    const int r = g.emit(root);
    if (g.textures.size() > 16) throw Error("GPU: too many textures for one shader");
    std::string s = "#version 430\nlayout(local_size_x = 16, local_size_y = 16) in;\n";
    s += "uniform ivec2 uSize;\nuniform ivec2 uOrigin;\nuniform ivec2 uFull;\nuniform bool uLinear;\n";
    s += kGlslCommon;
    if (!g.lut.empty())
        s += "layout(std430, binding = 0) readonly buffer LutBuffer { float L[]; };\n"
             "float lutLookupBase(int off, int n, float x) {\n"
             "    x = clamp(x, 0.0, 1.0) * float(n - 1);\n"
             "    int i = min(int(x), n - 2);\n"
             "    float f = x - float(i);\n"
             "    return L[off + i] + (L[off + i + 1] - L[off + i]) * f;\n}\n";
    s += g.code;
    std::vector<Format> formats;
    std::string stores;
    for (size_t k = 0; k < root.outImage.size(); ++k) {
        const std::string n = std::to_string(k);
        formats.push_back(outFormat(root.outImage[k], pd.half && !root.op.full));
        s += "layout(" + std::string(glslFormat(formats.back())) + ", binding = " + n + ") uniform writeonly image2D uOut" + n + ";\n";
        stores += "    imageStore(uOut" + n + ", p, " + (root.outImage[k] ? "o" + n : "vec4(o" + n + ")") + ");\n";
    }
    s += "void main() {\n    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n    if (p.x >= uSize.x || p.y >= uSize.y) return;\n    " +
         Generator::outDecls(root) + "\n    s" + std::to_string(r) + "_eval(p" + Generator::outArgs(root) + ");\n" + stores + "}\n";

    const unsigned prog = program(s);
    gl::UseProgram(prog);
    auto loc = [&](const std::string& name) { return gl::GetUniformLocation(prog, name.c_str()); };
    gl::Uniform2i(loc("uSize"), w, h);
    gl::Uniform2i(loc("uOrigin"), pd.frame.x0, pd.frame.y0);
    gl::Uniform2i(loc("uFull"), pd.frame.fullW, pd.frame.fullH);
    gl::Uniform1i(loc("uLinear"), pd.linear ? 1 : 0);
    for (const auto& [name, v] : g.floats) gl::Uniform1fv(loc(name), int(v.size()), v.data());
    for (const auto& [name, v] : g.ivec2s) gl::Uniform2i(loc(name), v.first, v.second);
    // Outputs first: a new texture is bound while it is created, which would replace an input's.
    std::vector<TexturePtr> outTex;
    for (Format f : formats) outTex.push_back(allocate(w, h, f));
    for (size_t u = 0; u < g.textures.size(); ++u) bindTexture(int(u), *g.textures[u]);
    gl::ActiveTexture(gl::TEXTURE0);
    for (size_t k = 0; k < outTex.size(); ++k) bindImage(int(k), *outTex[k]);
    unsigned lutBuf = 0;
    if (!g.lut.empty()) {
        gl::GenBuffers(1, &lutBuf);
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, lutBuf);
        gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(g.lut.size() * sizeof(float)), g.lut.data(), gl::STATIC_DRAW);
        gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 0, lutBuf);
    }
    dispatch(w, h);
    if (lutBuf) gl::DeleteBuffers(1, &lutBuf);  // freed once the dispatch is done with it
    if (gl::GetError() != gl::NO_ERROR) throw Error("GPU: point op failed");
    g_fused += g.stages - 1;
    return outTex;
}

// The input as a texture (running it if pending).
void useTexture(Stage::Input& in) {
    const GpuValue* g = nullptr;
    if (auto p = std::get_if<GpuImagePtr>(&in.src.v)) g = p->get(), in.kind = Kind::ImageTex;
    if (auto p = std::get_if<GpuChannelPtr>(&in.src.v)) g = p->get(), in.kind = Kind::ChannelTex;
    in.tex = g->texture().get();
    in.tw = g->w, in.th = g->h;
    in.fused.reset();
}

}  // namespace

void Pending::run() {
    Scope device;
    try {
        results = dispatchTree(*root, *this);
    } catch (const Error&) {
        // The fused shader failed (too big for the driver, too many textures): run the stages
        // feeding this one on their own first, then this one reading their textures.
        Stage alone = *root;
        bool any = false;
        for (Stage::Input& in : alone.inputs)
            if (in.kind == Kind::Fused) useTexture(in), any = true;
        if (!any) throw;
        results = dispatchTree(alone, *this);
    }
    root.reset();  // the inputs it held are no longer needed
}

namespace {

// runPoint and runPass: `info` gives the param ranges of pins (par<i>), when there is one.
std::vector<Value> runStage(EvalContext& ctx, const NodeInfo* info, const PointOp& op, const std::vector<Value>& in,
                            const std::vector<bool>& outImage) {
    const int w = op.w, h = op.h;
    if (w <= 0 || h <= 0) throw Error("GPU: empty output");

    auto st = std::make_shared<Stage>();
    st->op = op;
    st->outImage = outImage;
    st->inputs.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        Stage::Input& si = st->inputs[i];
        Value v = in[i];
        // CPU pixels are uploaded here when the evaluator hasn't already.
        if (auto p = std::get_if<ImagePtr>(&v.v); p && *p) v = Value(upload(**p, ctx.gpuHalf ? Format::RGBA16F : Format::RGBA32F));
        if (auto p = std::get_if<ChannelPtr>(&v.v); p && *p && !(*p)->constant) v = Value(upload(**p));
        const GpuValue* g = nullptr;
        if (auto p = std::get_if<GpuImagePtr>(&v.v); p && *p) g = p->get();
        if (auto p = std::get_if<GpuChannelPtr>(&v.v); p && *p) g = p->get();
        if (g) {
            si.src = v;
            const bool gather = std::find(op.gather.begin(), op.gather.end(), int(i)) != op.gather.end();
            if (!g->tex && g->pending && g->pending->root && g->w == w && g->h == h && !gather && g->pending->root->op.inlinable) {
                // Fusion: the producer hasn't run, and lines up pixel for pixel with this op, so
                // its code runs inside this shader instead of writing a texture.
                si.kind = Kind::Fused;
                si.fused = g->pending->root;
                si.fusedOut = g->pendingOut;
            } else {
                useTexture(si);
            }
        } else if (auto p = std::get_if<float>(&v.v)) {
            si.kind = Kind::Constant;
            si.constant = *p;
        } else if (auto p = std::get_if<ChannelPtr>(&v.v); p && *p) {
            si.kind = Kind::Constant;
            si.constant = (*p)->value;
        } else if (v.empty() && i < op.defaults.size() && !std::isnan(op.defaults[i])) {
            si.kind = Kind::Constant;
            si.constant = op.defaults[i];
        }
        // paramSampler's range: the param backing the pin, if any.
        if (info && i < info->inputs.size() && info->inputs[i].fallbackParam >= 0) {
            const ParamDesc& d = info->params[size_t(info->inputs[i].fallbackParam)];
            si.lo = d.hardMin;
            si.hi = d.hardMax;
        }
    }

    // Nothing runs yet: the outputs are pending until a texture of them is needed.
    auto pd = std::make_shared<Pending>();
    pd->root = st;
    pd->frame = nodeutil::frameOf(ctx, w, h);
    pd->linear = ctx.linear();
    pd->half = ctx.gpuHalf;
    std::vector<Value> out(st->outImage.size());
    for (size_t k = 0; k < st->outImage.size(); ++k) {
        if (st->outImage[k]) {
            auto im = std::make_shared<GpuImage>();
            im->pending = pd, im->pendingOut = int(k), im->w = w, im->h = h;
            out[k] = Value(GpuImagePtr(im));
        } else {
            auto c = std::make_shared<GpuChannel>();
            c->pending = pd, c->pendingOut = int(k), c->w = w, c->h = h;
            out[k] = Value(GpuChannelPtr(c));
        }
    }
    return out;
}

}  // namespace

void runPoint(EvalContext& ctx, const Node& node, const PointOp& op, const std::vector<Value>& in,
              std::vector<Value>& out) {
    const NodeInfo& info = node.info();
    std::vector<bool> outImage;
    for (const PinDesc& o : info.outputs) {
        if (o.type != PinType::Image && o.type != PinType::Channel) throw Error("GPU: point ops output images and channels");
        outImage.push_back(o.type == PinType::Image);
    }
    out = runStage(ctx, &info, op, in, outImage);
}

std::vector<Value> runPass(EvalContext& ctx, const PointOp& op, const std::vector<Value>& in,
                           const std::vector<bool>& outImage) {
    return runStage(ctx, nullptr, op, in, outImage);
}

void runOver(EvalContext& ctx, const Node& node, PointOp op, const std::vector<Value>& in, std::vector<Value>& out,
             int pin) {
    if (!in[size_t(pin)].size(op.w, op.h)) throw Error("GPU: no image to process");
    runPoint(ctx, node, op, in, out);
}

int fusedStages() { return g_fused.load(); }

bool pending(const Value& v) {
    const GpuValue* g = nullptr;
    if (auto p = std::get_if<GpuImagePtr>(&v.v); p && *p) g = p->get();
    if (auto p = std::get_if<GpuChannelPtr>(&v.v); p && *p) g = p->get();
    return g && !g->tex && g->pending;
}

void materialize(const Value& v) {
    if (auto p = std::get_if<GpuImagePtr>(&v.v); p && *p) (*p)->texture();
    if (auto p = std::get_if<GpuChannelPtr>(&v.v); p && *p) (*p)->texture();
}

}  // namespace gpu

const std::shared_ptr<gpu::Texture>& GpuValue::texture() const {
    if (!tex && pending) {
        if (pending->results.empty()) pending->run();
        tex = pending->results[size_t(pendingOut)];
        pending.reset();
    }
    return tex;
}
