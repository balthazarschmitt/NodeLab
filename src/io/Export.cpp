#include "io/Export.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <future>
#include <numbers>
#include <optional>
#include <set>

#include "core/ColorMath.h"
#include "core/OutputSpace.h"
#include "core/Parallel.h"
#include "gpu/Device.h"
#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/Exif.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Library.h"
#include "nodes/ImageOps.h"
#include "io/Paths.h"
#include "io/RawDecode.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"

namespace fs = std::filesystem;

SaveOptions ExportSettings::saveOptions(const std::string& source, int w, int h) const {
    SaveOptions o;
    o.format = FileFormat(format);
    o.depth = depth;
    o.jpegQuality = jpegQuality;
    o.lossless = lossless;
    if (format == JPEG || format == WEBP || format == JXL || format == AVIF) o.exif = exif::exportBlock(source, w, h);
    o.space = outspace::valid(colorSpace) ? colorSpace : 0;
    // HDR needs a format that can say PQ (PNG's cICP chunk, JPEG XL's colour encoding, AVIF's
    // CICP), at 16 bits (AVIF 10); other formats get Rec.2020.
    if (outspace::isHdr(o.space)) {
        if (hasHdr(FileFormat(format))) o.depth = 16;
        else o.space = outspace::Rec2020;
    }
    return o;
}

nlohmann::json ExportSettings::toJson() const {
    return {{"format", format},     {"depth", depth},           {"jpegQuality", jpegQuality}, {"lossless", lossless},
            {"sizeMode", sizeMode}, {"longEdge", longEdge},     {"percent", percent}, {"highQuality", highQuality},
            {"fileOutputs", fileOutputs}, {"nameTemplate", nameTemplate},   {"sharpenFor", sharpenFor},
            {"sharpenAmount", sharpenAmount}, {"colorSpace", colorSpace}};
}

void ExportSettings::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    format = std::clamp(j.value("format", format), 0, kFileFormatCount - 1);
    depth = std::clamp(j.value("depth", depth), 8, 32);
    jpegQuality = std::clamp(j.value("jpegQuality", jpegQuality), 1, 100);
    if (auto l = j.find("lossless"); l != j.end() && l->is_boolean()) lossless = l->get<bool>();
    sizeMode = std::clamp(j.value("sizeMode", sizeMode), 0, 2);
    longEdge = std::clamp(j.value("longEdge", longEdge), 16, 65536);
    percent = std::clamp(j.value("percent", percent), 1, 100);
    if (auto q = j.find("highQuality"); q != j.end() && q->is_boolean()) highQuality = q->get<bool>();
    fileOutputs = j.value("fileOutputs", fileOutputs);
    sharpenFor = std::clamp(j.value("sharpenFor", sharpenFor), 0, 3);
    sharpenAmount = std::clamp(j.value("sharpenAmount", sharpenAmount), 0, 2);
    colorSpace = std::clamp(j.value("colorSpace", colorSpace), 0, int(outspace::kCount) - 1);
    // Before templates there was only a suffix after the source's name.
    if (auto t = j.find("nameTemplate"); t != j.end() && t->is_string() && !t->get<std::string>().empty())
        nameTemplate = t->get<std::string>();
    else if (auto sfx = j.find("suffix"); sfx != j.end() && sfx->is_string())
        nameTemplate = "{name}" + sfx->get<std::string>();
}

bool ExportSettings::sameOutput(const ExportSettings& o) const {
    nlohmann::json a = toJson(), b = o.toJson();
    a.erase("fileOutputs");
    b.erase("fileOutputs");
    // The depth means nothing for JPEG and WebP, the quality nothing for lossless files, and
    // lossless nothing for formats without it.
    if (format == JPEG || format == WEBP) a.erase("depth"), b.erase("depth");
    if (sizeMode == Original) a.erase("highQuality"), b.erase("highQuality");  // nothing is resized
    if (!hasLossless(FileFormat(format))) a.erase("lossless"), b.erase("lossless");
    if (!hasQuality(FileFormat(format)) || (hasLossless(FileFormat(format)) && lossless && o.lossless))
        a.erase("jpegQuality"), b.erase("jpegQuality");
    return a == b;
}

const std::vector<ExportPreset>& builtInExportPresets() {
    static const std::vector<ExportPreset> presets = [] {
        std::vector<ExportPreset> v;
        auto add = [&](const char* name, auto&& set) {
            ExportPreset p{name, {}};
            set(p.settings);
            v.push_back(std::move(p));
        };
        add("Full-Size JPEG", [](ExportSettings& s) { s.format = ExportSettings::JPEG; });
        add("Web JPEG (2048 px)", [](ExportSettings& s) {
            s.format = ExportSettings::JPEG, s.jpegQuality = 85;
            s.sizeMode = ExportSettings::LongEdge, s.longEdge = 2048;
            s.sharpenFor = ExportSettings::Screen;
        });
        add("Email (1000 px)", [](ExportSettings& s) {
            s.format = ExportSettings::JPEG, s.jpegQuality = 75;
            s.sizeMode = ExportSettings::LongEdge, s.longEdge = 1000;
            s.sharpenFor = ExportSettings::Screen;
        });
        add("Full-Size PNG", [](ExportSettings& s) { s.format = ExportSettings::PNG; });
        add("16-bit TIFF", [](ExportSettings& s) { s.format = ExportSettings::TIFF, s.depth = 16; });
        add("OpenEXR (Half Float)", [](ExportSettings& s) { s.format = ExportSettings::EXR, s.depth = 16; });
        return v;
    }();
    return presets;
}

namespace {

float lanczos3(double x) {
    x = std::abs(x);
    if (x < 1e-8) return 1.0f;
    if (x >= 3.0) return 0.0f;
    const double px = std::numbers::pi * x;
    return float(3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px));
}

// Four floats in one register (GCC's vector extension).
using V4 = float __attribute__((vector_size(16)));

// Per output sample: the first source sample and `stride` weights (zero-padded), normalised.
struct Taps {
    std::vector<int> first;
    std::vector<float> w;
    int stride = 0;
};

Taps lanczosTaps(int n, int m) {
    const double scale = double(m) / n;
    const double fs = std::min(scale, 1.0);  // downscaling widens the kernel by 1/scale
    const double support = 3.0 / fs;
    Taps t;
    t.stride = int(std::ceil(support * 2)) + 2;
    t.first.resize(m);
    t.w.assign(size_t(m) * t.stride, 0.0f);
    for (int i = 0; i < m; ++i) {
        const double center = (i + 0.5) / scale;  // in source pixels
        const int lo = std::max(0, int(std::floor(center - support)));
        const int hi = std::min(n - 1, int(std::ceil(center + support)));
        t.first[i] = lo;
        float* w = &t.w[size_t(i) * t.stride];
        double sum = 0;
        for (int j = lo; j <= hi && j - lo < t.stride; ++j) sum += w[j - lo] = lanczos3((j + 0.5 - center) * fs);
        // Taps cut off at the image edge are dropped, so renormalise.
        if (sum != 0)
            for (int k = 0; k < t.stride; ++k) w[k] = float(w[k] / sum);
    }
    return t;
}

}  // namespace

std::shared_ptr<Image> resizeLanczos(const Image& src, int w, int h, bool srgbEncoded) {
    w = std::max(1, w);
    h = std::max(1, h);
    const Taps tx = lanczosTaps(src.w, w), ty = lanczosTaps(src.h, h);

    // Horizontal pass into src.h rows of w pixels. Each source row is first converted to linear
    // light, colour premultiplied by alpha, in a buffer of its own: a converted copy of the whole
    // source would be as big as the source and cost a pass over it.
    // The taps without zero weights (padding, and taps past the edge), as offsets into a row,
    // so the inner loop has no branches.
    std::vector<int> hn(w), hoff(size_t(w) * tx.stride);
    std::vector<float> hw(size_t(w) * tx.stride);
    for (int x = 0; x < w; ++x) {
        const size_t base = size_t(x) * tx.stride;
        int n = 0;
        for (int k = 0; k < tx.stride && tx.first[x] + k < src.w; ++k)
            if (tx.w[base + k] != 0.0f) {
                hoff[base + n] = (tx.first[x] + k) * 4;
                hw[base + n++] = tx.w[base + k];
            }
        hn[x] = n;
    }
    Image mid(w, src.h);
    parallelFor(src.h, [&](int y) {
        thread_local std::vector<float> buf;
        buf.resize(size_t(src.w) * 4);
        for (int x = 0; x < src.w; ++x) {
            const float* s = src.pixel(size_t(y) * src.w + x);
            float* d = &buf[size_t(x) * 4];
            const float a = std::clamp(s[3], 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) d[c] = (srgbEncoded ? colormath::srgbToLinear(s[c]) : s[c]) * a;
            d[3] = a;
        }
        const float* row = buf.data();
        for (int x = 0; x < w; ++x) {
            const int* off = &hoff[size_t(x) * tx.stride];
            const float* wt = &hw[size_t(x) * tx.stride];
            // A pixel's four channels at once (the same sums and comparisons as one at a time).
            V4 acc = {0, 0, 0, 0}, lo = {INFINITY, INFINITY, INFINITY, INFINITY}, hi = -lo;
            for (int k = 0, n = hn[x]; k < n; ++k) {
                V4 p;
                std::memcpy(&p, row + off[k], sizeof p);
                acc += wt[k] * p;
                lo = p < lo ? p : lo;  // std::min, std::max
                hi = hi < p ? p : hi;
            }
            const V4 below = hi < acc ? hi : acc;  // std::clamp
            acc = acc < lo ? lo : below;
            std::memcpy(mid.pixel(size_t(y) * w + x), &acc, sizeof acc);
        }
    });

    // Vertical pass, a whole output row at a time so the reads run along rows.
    auto out = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        const float* wt = &ty.w[size_t(y) * ty.stride];
        std::vector<float> acc(size_t(w) * 4, 0.0f), lo(size_t(w) * 4, INFINITY), hi(size_t(w) * 4, -INFINITY);
        for (int k = 0; k < ty.stride && ty.first[y] + k < src.h; ++k) {
            if (wt[k] == 0.0f) continue;
            const float* row = mid.pixel(size_t(ty.first[y] + k) * w);
            for (size_t i = 0; i < size_t(w) * 4; ++i) {
                acc[i] += wt[k] * row[i];
                lo[i] = std::min(lo[i], row[i]);
                hi[i] = std::max(hi[i], row[i]);
            }
        }
        for (int x = 0; x < w; ++x) {
            float* d = out->pixel(size_t(y) * w + x);
            const size_t i = size_t(x) * 4;
            const float a = std::clamp(acc[i + 3], lo[i + 3], hi[i + 3]);
            d[3] = a;
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(acc[i + c], lo[i + c], hi[i + c]);
                const float un = a > 0.0f ? v / a : 0.0f;
                d[c] = srgbEncoded ? colormath::linearToSrgb(un) : un;
            }
        }
    });
    return out;
}

void exportSize(int w, int h, const ExportSettings& s, int& ow, int& oh) {
    ow = w;
    oh = h;
    if (s.sizeMode == ExportSettings::Original || w <= 0 || h <= 0) return;
    const int edge = std::max(w, h);
    const int target = std::max(1, s.sizeMode == ExportSettings::LongEdge
                                       ? s.longEdge
                                       : int(std::lround(edge * std::clamp(s.percent, 1, 100) / 100.0)));
    if (edge <= target) return;  // never enlarge
    const double scale = double(target) / edge;
    ow = std::max(1, int(std::lround(w * scale)));
    oh = std::max(1, int(std::lround(h * scale)));
}

std::shared_ptr<const Image> resizeForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                             bool srgbEncoded) {
    if (!img) return img;
    int w, h;
    exportSize(img->w, img->h, s, w, h);
    if (w == img->w && h == img->h) return img;
    return resizeLanczos(*img, w, h, srgbEncoded);
}

std::shared_ptr<const Image> sharpenForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                              bool linear) {
    if (!img || img->empty() || s.sharpenFor <= ExportSettings::SharpenOff || s.sharpenFor > ExportSettings::Glossy)
        return img;
    // Screen wants a fine radius (pixels are seen one to one); prints spread ink, so paper wants
    // a wider one, and matte paper, which softens more than glossy, the strongest.
    static const float kRadius[4] = {0.0f, 0.6f, 1.0f, 0.8f};
    static const float kAmount[4][3] = {{0, 0, 0}, {25, 45, 75}, {40, 65, 100}, {30, 55, 85}};
    imageops::SharpenSettings st;
    st.radius = kRadius[s.sharpenFor];
    st.amount = kAmount[s.sharpenFor][std::clamp(s.sharpenAmount, 0, 2)];
    st.detail = 50.0f;
    auto out = std::make_shared<Image>(*img);
    imageops::sharpenImage(*out, st, linear);
    if (!linear)
        for (size_t i = 0; i < out->px.size(); i += 4)
            for (int c = 0; c < 3; ++c) out->px[i + c] = std::min(out->px[i + c], 1.0f);
    return out;
}

bool saveRendered(const std::string& pathU8, const std::shared_ptr<const Image>& scene, const ColorManagement& cm,
                  const SaveOptions& opt, std::string& err) {
    if (!scene) {
        err = "nothing to save";
        return false;
    }
    if (opt.format != FileFormat::EXR) return writeImage(pathU8, *displayInSpace(scene, cm, opt.space), opt, err);
    if (cm.linear) return writeImage(pathU8, *scene, opt, err);
    // Legacy projects hold sRGB-encoded values; EXR stores linear light.
    Image lin(scene->w, scene->h);
    parallelFor(scene->h, [&](int y) {
        for (int x = 0; x < scene->w; ++x) {
            const size_t i = size_t(y) * scene->w + x;
            const float* s = scene->pixel(i);
            float* d = lin.pixel(i);
            for (int c = 0; c < 3; ++c) d[c] = colormath::srgbToLinear(s[c]);
            d[3] = s[3];
        }
    });
    return writeImage(pathU8, lin, opt, err);
}

std::shared_ptr<const Image> displayInSpace(const std::shared_ptr<const Image>& scene, const ColorManagement& cm,
                                            int space) {
    if (!outspace::valid(space) || space == outspace::sRGB || !scene) return colormgmt::displayImage(scene, cm);
    const outspace::Mat3& m = outspace::fromRec709(space);
    const float exposure = std::exp2(cm.exposure);
    auto out = std::make_shared<Image>(scene->w, scene->h);
    if (outspace::isHdr(space)) {
        parallelFor(scene->h, [&](int y) {
            for (int x = 0; x < scene->w; ++x) {
                const size_t i = size_t(y) * scene->w + x;
                const float* s = scene->pixel(i);
                float lin[3], t[3];
                for (int c = 0; c < 3; ++c) {
                    const float v = std::isfinite(s[c]) ? s[c] : 0.0f;
                    lin[c] = (cm.linear ? v : colormath::srgbToLinear(std::clamp(v, 0.0f, 1.0f))) * exposure;
                }
                outspace::apply(m, lin, t);
                float* d = out->pixel(i);
                for (int c = 0; c < 3; ++c) d[c] = outspace::pqEncode(std::max(t[c], 0.0f) * outspace::kHdrReferenceWhite);
                d[3] = s[3];
            }
        });
        return out;
    }
    const bool direct = cm.linear && cm.view == ColorManagement::Standard;
    // Other views tone map into sRGB's gamut: convert what they display.
    const std::shared_ptr<const Image> src = direct ? scene : colormgmt::displayImage(scene, cm);
    parallelFor(src->h, [&](int y) {
        for (int x = 0; x < src->w; ++x) {
            const size_t i = size_t(y) * src->w + x;
            const float* s = src->pixel(i);
            float lin[3], t[3];
            for (int c = 0; c < 3; ++c) {
                const float v = std::isfinite(s[c]) ? s[c] : 0.0f;
                if (direct) lin[c] = v * exposure;
                else lin[c] = colormath::srgbToLinear(std::clamp(v, 0.0f, 1.0f));
            }
            // The display gamma was applied to sRGB values by displayImage; as Standard's own step
            // here, after encoding.
            outspace::apply(m, lin, t);
            float* d = out->pixel(i);
            for (int c = 0; c < 3; ++c) {
                d[c] = outspace::encode(space, t[c]);
                if (direct && cm.gamma != 1.0f) d[c] = std::pow(d[c], 1.0f / cm.gamma);
            }
            d[3] = s[3];
        }
    });
    return out;
}

std::string metadataSource(const Graph& g) {
    for (const auto& [id, n] : g.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && !n->paramS(0).empty()) return n->paramS(0);
    return {};
}

const NameToken kNameTokens[] = {
    {"{name}", "File name, without extension"},
    {"{seq}", "Sequence number (1, 2, 3...)"},
    {"{seq:3}", "Sequence number, 3 digits (001)"},
    {"{date}", "Capture date (YYYY-MM-DD)"},
    {"{time}", "Capture time (HHMMSS)"},
    {"{year}", "Capture year"},
    {"{month}", "Capture month (01-12)"},
    {"{day}", "Capture day (01-31)"},
    {"{camera}", "Camera model"},
    {"{make}", "Camera make"},
    {"{lens}", "Lens"},
    {"{iso}", "ISO"},
    {"{focal}", "Focal length (50mm)"},
    {"{aperture}", "Aperture (f2.8)"},
    {"{shutter}", "Shutter speed (1-250s)"},
    {"{copy}", "Virtual copy name (Copy 1)"},
    {"{folder}", "Folder name"},
    {"{today}", "Export date (YYYY-MM-DD)"},
};
const int kNameTokenCount = int(std::size(kNameTokens));

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// "%.*g": no trailing ".0".
std::string number(double v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*g", digits, v);
    return buf;
}

// Windows forbids <>:"/\|?* and control characters in names, and trailing dots or spaces.
std::string sanitizeName(std::string s) {
    for (char& c : s)
        if ((unsigned char)c < 32 || std::strchr("<>:\"/\\|?*", c)) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

// A time as EXIF writes it, "YYYY:MM:DD HH:MM:SS".
std::string exifTime(std::time_t t) {
    std::tm tm{};
    char buf[20];
    if (localtime_s(&tm, &t) != 0 || !std::strftime(buf, sizeof buf, "%Y:%m:%d %H:%M:%S", &tm)) return {};
    return buf;
}

}  // namespace

std::string presetFolder(const std::string& outDirU8, const std::string& presetName) {
    std::string name = sanitizeName(presetName);
    if (name.empty() || name == "..") name = "Preset";
    return pathToU8(u8ToPath(outDirU8) / u8ToPath(name));
}

std::string expandNameTemplate(const std::string& tmpl, const NameSource& src) {
    const fs::path path = u8ToPath(src.path);
    // EXIF is read only when the template asks for it (a RAW that isn't TIFF-based is read whole).
    std::optional<exif::PhotoInfo> info;
    auto meta = [&]() -> const exif::PhotoInfo& {
        if (!info) {
            info.emplace();
            if (!src.path.empty()) exif::readInfo(src.path, *info);
        }
        return *info;
    };
    // The capture time, else the file's modification time (as Lightroom falls back to it).
    std::optional<std::string> when;
    auto captured = [&]() -> const std::string& {
        if (!when) {
            when = meta().captureTime;
            if (when->size() < 19 && !src.path.empty()) {
                std::error_code ec;
                const auto ft = fs::last_write_time(path, ec);
                if (!ec)
                    when = exifTime(std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(ft)));
            }
            if (when->size() < 19) when = std::string();
        }
        return *when;
    };
    auto part = [&](size_t at, size_t len) { return captured().size() >= at + len ? captured().substr(at, len) : std::string(); };

    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        const size_t close = tmpl[i] == '{' ? tmpl.find('}', i) : std::string::npos;
        if (close == std::string::npos) {
            out += tmpl[i++];
            continue;
        }
        const std::string raw = tmpl.substr(i, close - i + 1);
        std::string key = lowerAscii(raw.substr(1, raw.size() - 2));
        int pad = 0;
        if (const size_t colon = key.find(':'); colon != std::string::npos) {
            pad = std::clamp(std::atoi(key.c_str() + colon + 1), 0, 9);
            key.resize(colon);
        }
        std::string v;
        bool known = true;
        if (key == "name") v = src.path.empty() ? "export" : pathToU8(path.stem());
        else if (key == "folder") v = pathToU8(path.parent_path().filename());
        else if (key == "seq") {
            v = std::to_string(src.sequence);
            if (int(v.size()) < pad) v.insert(0, size_t(pad) - v.size(), '0');
        } else if (key == "copy") v = src.copy > 0 ? "Copy " + std::to_string(src.copy) : "";
        else if (key == "date") v = captured().empty() ? "" : part(0, 4) + "-" + part(5, 2) + "-" + part(8, 2);
        else if (key == "year") v = part(0, 4);
        else if (key == "month") v = part(5, 2);
        else if (key == "day") v = part(8, 2);
        else if (key == "hour") v = part(11, 2);
        else if (key == "minute") v = part(14, 2);
        else if (key == "second") v = part(17, 2);
        else if (key == "time") v = part(11, 2) + part(14, 2) + part(17, 2);
        else if (key == "today") {
            const std::string t = exifTime(std::time(nullptr));
            v = t.size() >= 10 ? t.substr(0, 4) + "-" + t.substr(5, 2) + "-" + t.substr(8, 2) : "";
        } else if (key == "camera") v = meta().model;
        else if (key == "make") v = meta().make;
        else if (key == "lens") v = meta().lens;
        else if (key == "iso") v = meta().iso > 0 ? number(meta().iso, 6) : "";
        else if (key == "focal") v = meta().focalLength > 0 ? number(meta().focalLength, 4) + "mm" : "";
        else if (key == "aperture") v = meta().fNumber > 0 ? "f" + number(meta().fNumber, 2) : "";
        else if (key == "shutter") {
            const float t = meta().exposureTime;
            v = t <= 0 ? "" : t < 0.4f ? "1-" + number(std::round(1.0 / t), 6) + "s" : number(t, 3) + "s";
        } else known = false;
        out += known ? v : raw;
        i = close + 1;
    }
    out = sanitizeName(out);
    return out.empty() ? sanitizeName(src.path.empty() ? "export" : pathToU8(path.stem())) : out;
}

std::string batchOutputPath(const std::string& sourceU8, const std::string& outDirU8, const ExportSettings& s,
                            int sequence, int copy) {
    const auto src = u8ToPath(sourceU8);
    const std::string name = expandNameTemplate(s.nameTemplate, {sourceU8, sequence, copy});
    auto out = u8ToPath(outDirU8) / u8ToPath(name + s.extension());
    // A template naming the file as it is, into the source folder, would overwrite the original.
    std::error_code ec;
    if (std::filesystem::equivalent(out, src, ec) || out.lexically_normal() == src.lexically_normal())
        out = u8ToPath(outDirU8) / u8ToPath(name + "_edit" + s.extension());
    return pathToU8(out);
}

std::vector<std::string> batchOutputPaths(const std::vector<NameSource>& sources, const std::string& outDirU8,
                                          const ExportSettings& s) {
    std::vector<std::string> out;
    std::error_code ec;
    // Lower case: Windows names ignore case.
    auto key = [&](const fs::path& p) { return lowerAscii(pathToU8(fs::absolute(p, ec).lexically_normal())); };
    // sourceNames: each source without its extension, the name a RAW+JPEG pair shares.
    std::set<std::string> used, sourceFiles, sourceNames;
    for (const NameSource& src : sources)
        if (!src.path.empty()) {
            sourceFiles.insert(key(u8ToPath(src.path)));
            sourceNames.insert(key(u8ToPath(src.path).replace_extension()));
        }
    // A file already there is an earlier export, which a new export replaces, unless it's an
    // original: a Library photo (it has a sidecar), or the camera's JPEG beside a RAW. That one
    // has the RAW's name and the camera's EXIF, where Refractory's export of the RAW has its own
    // Software tag (and exports of PNGs and TIFFs have no camera EXIF).
    auto original = [&](const fs::path& p) {
        if (fs::exists(u8ToPath(library::sidecarPath(pathToU8(p))), ec)) return true;
        if (!sourceNames.count(key(fs::path(p).replace_extension()))) return false;
        exif::PhotoInfo info;
        return exif::readInfo(pathToU8(p), info) && !info.make.empty() && info.software.rfind("Refractory", 0) != 0 &&
               info.software.rfind("NodeLab", 0) != 0;
    };
    auto taken = [&](const fs::path& p) {
        if (used.count(key(p)) || sourceFiles.count(key(p))) return true;
        return fs::exists(p, ec) && original(p);
    };
    for (size_t i = 0; i < sources.size(); ++i) {
        fs::path p = u8ToPath(batchOutputPath(sources[i].path, outDirU8, s, int(i) + 1, sources[i].copy));
        if (taken(p)) {
            const fs::path base = p;
            for (int k = 2;; ++k) {
                p = base.parent_path() / u8ToPath(pathToU8(base.stem()) + " (" + std::to_string(k) + ")" + pathToU8(base.extension()));
                if (!taken(p)) break;
            }
        }
        used.insert(key(p));
        out.push_back(pathToU8(p));
    }
    return out;
}

Exporter::~Exporter() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void Exporter::start(const nlohmann::json& graph, std::vector<ExportItem> items, int inputNode, const ExportSettings& s,
                     bool gpu) {
    if (thread_.joinable()) thread_.join();  // a finished job's thread
    cancel_ = false;
    busy_ = true;
    {
        std::lock_guard lock(mutex_);
        progress_ = {};
        progress_.total = int(items.size());
    }
    thread_ = std::thread([this, graph, items = std::move(items), inputNode, s, gpu]() mutable {
        run(std::move(graph), std::move(items), inputNode, std::move(s), gpu);
    });
}

void Exporter::wait() {
    if (thread_.joinable()) thread_.join();
}

Exporter::Progress Exporter::progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

std::vector<std::string> Exporter::takeLog() {
    std::lock_guard lock(mutex_);
    return std::exchange(log_, {});
}

void Exporter::setStage(const std::string& s) {
    std::lock_guard lock(mutex_);
    progress_.stage = s;
}

void Exporter::log(const std::string& line) {
    std::lock_guard lock(mutex_);
    log_.push_back(line);
}

namespace {

// Scales an image down so its long edge is at most `edge`, with the export's Lanczos filter (a
// preview proxy's box filter would soften and alias the file).
ImagePtr fitLanczos(const ImagePtr& img, int edge) {
    if (!img || std::max(img->w, img->h) <= edge) return img;
    const double scale = double(edge) / std::max(img->w, img->h);
    return resizeLanczos(*img, std::max(1, int(std::lround(img->w * scale))), std::max(1, int(std::lround(img->h * scale))));
}

// Whether an item's files can come from a render at reduced size: downsized files, unless one asks
// for high quality resampling. Legacy projects keep the full render, so their files stay as they
// were, and so do File Output nodes, which are written at full size from the same evaluation.
bool canReduce(const Graph& g, const std::vector<const ExportSettings*>& sets, bool fileOutputs) {
    bool reduced = g.colorManagement.linear && !sets.empty() && !(fileOutputs && hasFileOutputs(g));
    for (const ExportSettings* set : sets)
        reduced = reduced && set->sizeMode != ExportSettings::Original && !set->highQuality;
    return reduced;
}

// The Image Input whose file sets an item's size: the batch's, or the first with a file.
const ImageInputNode* primaryInput(const Graph& g, int inputNode) {
    for (const auto& [id, n] : g.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && (inputNode ? id == inputNode : !n->paramS(0).empty()))
            return static_cast<const ImageInputNode*>(n.get());
    return nullptr;
}

// loadImage, or a decode made ahead of time (see Exporter::run).
using DecodeFn = std::function<ImagePtr(const std::string& path, const ImageCache::Decode& decode, bool preview,
                                        int& fw, int& fh, std::string& err)>;

// Loads the graph's sources for a render at reduced size, the way a preview is rendered: each
// Image Input reads a proxy of long edge ctx.proxyEdge, about 1.5x the largest file's long edge.
// `primary` is the Image Input that sets the size (0: the first with a file); its full size goes to
// fullW/fullH. Returns false when the files need a full-resolution render instead (a file at the
// original size, or one too close to it to gain much); a full image already decoded is then left
// held in the cache, so it isn't decoded again.
bool loadReduced(const Graph& g, EvalContext& ctx, ImageCache& cache, int primary,
                 const std::vector<const ExportSettings*>& sets, const DecodeFn& load, int& fullW, int& fullH) {
    std::vector<const ImageInputNode*> inputs;
    bool found = !primary;
    for (const auto& [id, n] : g.nodes()) {
        if (n->info().type != ImageInputNode::staticInfo().type || n->paramS(0).empty()) continue;
        auto* in = static_cast<const ImageInputNode*>(n.get());
        if (id == primary) inputs.insert(inputs.begin(), in), found = true;
        else inputs.push_back(in);
    }
    if (inputs.empty() || !found) return false;
    int renderEdge = 0;
    for (const ImageInputNode* in : inputs) {
        const std::string& path = in->paramS(0);
        const ImageCache::Decode decode = in->decode(ctx.linear());
        std::string err;
        int fw = 0, fh = 0;
        ImagePtr img;
        // A RAW's fast half-size decode is enough for most downsized files.
        const bool raw = raw::isRawPath(path);
        if (raw) img = load(path, decode, true, fw, fh, err);
        if (!renderEdge) {
            if (!raw) img = load(path, decode, false, fw, fh, err);
            if (!img || fw <= 0 || fh <= 0) return false;  // the full render reports the error
            const int edge = std::max(fw, fh);
            int need = 0;
            for (const ExportSettings* s : sets) {
                int w, h;
                exportSize(fw, fh, *s, w, h);
                need = std::max(need, std::max(w, h));
            }
            // Oversampled by half, so the file's own resize still antialiases and keeps detail.
            renderEdge = need * 3 / 2;
            if (renderEdge > edge * 4 / 5) {
                if (!raw) cache.put(path, decode, img, fw, fh);
                return false;
            }
            // A half-size RAW too small for the file: decode the whole thing.
            if (raw && std::max(img->w, img->h) < need * 5 / 4) img = load(path, decode, false, fw, fh, err);
            if (!img) return false;
            fullW = fw;
            fullH = fh;
        } else {
            if (raw && img && std::max(img->w, img->h) < renderEdge && std::max(img->w, img->h) < std::max(fw, fh))
                img = nullptr;
            if (!img) img = load(path, decode, false, fw, fh, err);
            if (!img) continue;  // the node reports it
        }
        ImagePtr proxy = fitLanczos(img, renderEdge);
        img.reset();
        cache.put(path, decode, proxy, fw, fh, renderEdge);
        if (in == inputs.front()) {
            ctx.defaultW = proxy->w;
            ctx.defaultH = proxy->h;
            ctx.scale = float(proxy->w) / fw;
        }
    }
    ctx.proxy = true;
    ctx.proxyEdge = renderEdge;
    return true;
}

}  // namespace

void Exporter::run(nlohmann::json graphJson, std::vector<ExportItem> items, int inputNode, ExportSettings s, bool gpu) {
    Graph jobGraph;
    try {
        if (!graphJson.is_null()) jobGraph.fromJson(graphJson);
    } catch (const std::exception& e) {
        log(std::string("Export failed: ") + e.what());
        busy_ = false;
        return;
    }
    const bool batch = inputNode != 0;
    int failed = 0;
    auto setsOf = [&](const ExportItem& item) {
        std::vector<const ExportSettings*> sets;
        if (!item.output.empty()) sets.push_back(&s);
        for (const ExportItem::Extra& e : item.extras) sets.push_back(&e.settings);
        return sets;
    };
    // The next item's main source decodes on another thread while this one renders and saves:
    // much of a RAW decode (unpacking the file) runs on one core. Its graph is set up the same
    // way on a copy; a decode that turns out not to match is simply not used.
    struct Decoded {
        std::string path;
        ImageCache::Decode decode;
        bool preview = false;
        ImagePtr img;
        int fw = 0, fh = 0;
        std::string err;
    };
    Graph probe;
    if (batch) probe.fromJson(graphJson);
    auto prefetch = [&](size_t j) -> std::future<Decoded> {
        const ExportItem& item = items[j];
        Graph own;
        const Graph* g = &probe;
        const ImageInputNode* in = nullptr;
        try {
            if (!item.graph.is_null()) {
                own.fromJson(item.graph);
                g = &own;
                in = primaryInput(own, 0);
            } else if (batch && (in = primaryInput(probe, inputNode))) {
                // As the item's own render will (chooseFile can change the decode settings).
                const_cast<ImageInputNode*>(in)->chooseFile(item.source);
            }
        } catch (const std::exception&) {
            return {};
        }
        if (!in) return {};
        Decoded d;
        d.path = in->paramS(0);
        d.decode = in->decode(g->colorManagement.linear);
        d.preview = canReduce(*g, setsOf(item), false) && raw::isRawPath(d.path);
        return std::async(std::launch::async, [d]() mutable {
            try {
                d.img = loadImage(d.path, d.err, d.decode, d.preview, &d.fw, &d.fh);
            } catch (const std::exception& e) {
                d.err = e.what();
            }
            return d;
        });
    };
    std::future<Decoded> next = items.size() > 1 ? prefetch(0) : std::future<Decoded>{};
    using Clock = std::chrono::steady_clock;
    auto seconds = [](Clock::time_point a, Clock::time_point b) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.2f s", std::chrono::duration<double>(b - a).count());
        return std::string(buf);
    };
    for (size_t i = 0; i < items.size() && !cancel_; ++i) {
        const ExportItem& item = items[i];
        // An item with its own edit (the library) renders that instead of the job's graph.
        Graph own;
        const bool ownGraph = !item.graph.is_null();
        if (ownGraph) {
            try {
                own.fromJson(item.graph);
            } catch (const std::exception& e) {
                ++failed;
                log("Failed " + item.source + ": " + e.what());
                continue;
            }
        }
        Graph& g = ownGraph ? own : jobGraph;
        const int outId = g.firstOfType(OutputNode::staticInfo().type);
        const std::string outName = pathToU8(u8ToPath(item.output).filename());
        // A fresh cache per item: batches would otherwise keep every source's preview in memory.
        ImageCache cache;
        EvalContext ctx;
        ctx.proxy = false;
        ctx.cache = &cache;
        ctx.cancel = &cancel_;
        ctx.colorManagement = g.colorManagement;
        ctx.gpu = gpu && gpu::available();
        ctx.gpuHalf = false;
        const Clock::time_point start = Clock::now();
        Clock::time_point loaded = start, rendered = start;
        Decoded pre = next.valid() ? next.get() : Decoded{};
        const DecodeFn decodeFile = [&](const std::string& path, const ImageCache::Decode& decode, bool preview, int& fw,
                                        int& fh, std::string& err) -> ImagePtr {
            if (pre.img && pre.path == path && pre.decode == decode && pre.preview == preview) {
                fw = pre.fw;
                fh = pre.fh;
                return std::exchange(pre.img, nullptr);
            }
            return loadImage(path, err, decode, preview, &fw, &fh);
        };
        try {
            Node* in = nullptr;
            if (batch && !ownGraph) {
                in = g.find(inputNode);
                if (!in) throw std::runtime_error("the batch Image Input node is gone");
                // As if chosen in the UI: a RAW batch from a JPEG project gets the RAW defaults.
                static_cast<ImageInputNode&>(*in).chooseFile(item.source);
                setStage("Loading " + pathToU8(u8ToPath(item.source).filename()));
            }
            // File Output nodes have fixed paths, so a batch would overwrite them on every item.
            const bool fileOutputs = !batch && !ownGraph && (s.fileOutputs || item.output.empty());
            const std::vector<const ExportSettings*> sets = setsOf(item);
            bool reduced = canReduce(g, sets, fileOutputs);
            int fullW = 0, fullH = 0;
            auto load = [&] {
                if (reduced) reduced = loadReduced(g, ctx, cache, in ? inputNode : 0, sets, decodeFile, fullW, fullH);
                if (!reduced) {
                    ctx.proxy = false;
                    // A full image decoded ahead goes in the cache, held for the node.
                    if (pre.img && !pre.preview) cache.put(pre.path, pre.decode, pre.img, pre.fw, pre.fh);
                }
                pre.img.reset();
                // Now the next item's decode can start.
                if (i + 1 < items.size() && !cancel_ && !next.valid()) next = prefetch(i + 1);
                if (reduced) return;
                if (in) {
                    std::string err;
                    // Held in the cache for the node, which would otherwise decode the file again.
                    ImagePtr src = cache.holdFull(item.source, &err, static_cast<const ImageInputNode&>(*in).decode(ctx.linear()));
                    if (!src) throw std::runtime_error(err.empty() ? "could not load " + item.source : err);
                    // Size from this source, not whichever Image Input happens to come first.
                    ctx.defaultW = src->w;
                    ctx.defaultH = src->h;
                    ctx.scale = 1.0f;
                } else {
                    initContextSize(g, ctx);
                }
            };
            Evaluator ev;
            // A full-resolution render needs only the outputs still to be read: dropping the rest
            // keeps a big photo's peak memory to a few images instead of one per node. File
            // Outputs read the graph again afterwards, so they keep everything.
            ev.releaseIntermediates = !fileOutputs;
            if (sets.empty()) {
                load();
                loaded = Clock::now();
            } else {
                ImagePtr img;
                int ew = 0, eh = 0;  // the render's size at full resolution
                for (;;) {
                    load();
                    loaded = Clock::now();
                    setStage("Rendering " + outName);
                    if (outId) {
                        // The device is held while evaluating only (the previews wait meanwhile),
                        // not while resizing and saving.
                        std::optional<gpu::Scope> device;
                        if (ctx.gpu) device.emplace();
                        img = ev.evaluateDisplay(g, outId, ctx);
                    }
                    if (!img) throw std::runtime_error("the Output node has no input");
                    rendered = Clock::now();
                    if (!reduced) {
                        ew = img->w;
                        eh = img->h;
                        break;
                    }
                    // Exact when the graph keeps its source's size; scaled back up otherwise (a crop).
                    if (img->w == ctx.defaultW && img->h == ctx.defaultH) {
                        ew = fullW;
                        eh = fullH;
                    } else {
                        ew = std::max(1, int(std::lround(img->w / ctx.scale)));
                        eh = std::max(1, int(std::lround(img->h / ctx.scale)));
                    }
                    bool enough = true;
                    for (const ExportSettings* set : sets) {
                        int w, h;
                        exportSize(ew, eh, *set, w, h);
                        enough = enough && std::max(w, h) <= std::max(img->w, img->h);
                    }
                    if (enough) break;
                    // A crop left less than the files need: render at full resolution after all.
                    reduced = false;
                    img.reset();
                }
                const std::string meta = batch || ownGraph ? item.source : metadataSource(g);
                // Every file comes from the one render: each resizes and sharpens it for itself.
                auto write = [&](const std::string& path, const ExportSettings& set) {
                    const Clock::time_point t0 = Clock::now();
                    // Before the view transform: resampling in scene light.
                    ImagePtr out = img;
                    int w, h;
                    exportSize(ew, eh, set, w, h);
                    if (w != img->w || h != img->h) out = resizeLanczos(*img, w, h, !ctx.linear());
                    out = sharpenForExport(out, set, ctx.linear());
                    const Clock::time_point t1 = Clock::now();
                    if (cancel_) return false;
                    setStage("Saving " + pathToU8(u8ToPath(path).filename()));
                    std::error_code ec;
                    if (u8ToPath(path).has_parent_path()) fs::create_directories(u8ToPath(path).parent_path(), ec);
                    std::string err;
                    SaveOptions opt = set.saveOptions(meta, out->w, out->h);
                    opt.xmp = item.xmp;
                    if (!saveRendered(path, out, ctx.colorManagement, opt, err)) throw std::runtime_error(err);
                    std::string line = "Wrote " + path + " (" + std::to_string(out->w) + " x " + std::to_string(out->h) + ")";
                    if (timings)
                        line += ": load " + seconds(start, loaded) + ", render " + seconds(loaded, rendered) + ", resize " +
                                seconds(t0, t1) + ", save " + seconds(t1, Clock::now()) + (reduced ? " (reduced render)" : "");
                    log(line);
                    return true;
                };
                if (!item.output.empty() && !write(item.output, s)) break;
                bool stopped = false;
                for (const ExportItem::Extra& e : item.extras)
                    if (!write(e.output, e.settings)) {
                        stopped = true;
                        break;
                    }
                if (stopped) break;
            }
            if (fileOutputs) {
                setStage("Writing File Outputs");
                for (const std::string& line : writeFileOutputs(g, ev, ctx)) log(line);
            }
            if (ev.gpuFallbacks)
                log(std::to_string(ev.gpuFallbacks) + " nodes ran on the CPU instead: " + ev.lastGpuError);
        } catch (const EvalCancelled&) {
            break;
        } catch (const std::exception& e) {
            ++failed;
            log("Failed " + (batch ? item.source : outName) + ": " + e.what());
        }
        if (ctx.gpu) {
            // A full-resolution image's textures are hundreds of MB: keep only a preview's worth.
            gpu::Scope device;
            gpu::trimPool(size_t(128) << 20);
        }
        std::lock_guard lock(mutex_);
        progress_.done = int(i + 1);
        progress_.failed = failed;
    }
    {
        std::lock_guard lock(mutex_);
        progress_.cancelled = cancel_;
        progress_.stage = cancel_ ? "Cancelled" : "Done";
    }
    if (cancel_) log("Cancelled");
    busy_ = false;
}
