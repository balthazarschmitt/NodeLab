#include "io/ImageIO.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include <stb_image_write.h>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "io/Exif.h"
#include "io/ExrDecode.h"
#include "io/Icc.h"
#include "io/Paths.h"
#include "io/JpegDecode.h"
#include "io/PngDecode.h"
#include "io/RawDecode.h"
#include "io/TiffDecode.h"

namespace {

// Code value -> float for every value of an 8- or 16-bit channel, so decoding is one lookup.
std::vector<float> decodeTable(int levels, bool srgbToLinear, const icc::Curve* curve = nullptr) {
    std::vector<float> t(static_cast<size_t>(levels));
    for (int i = 0; i < levels; ++i) {
        const float v = float(i) / float(levels - 1);
        t[size_t(i)] = curve ? curve->eval(v) : srgbToLinear ? float(colormath::srgbToLinear(v)) : v;
    }
    return t;
}

// With a profile, each channel decodes through its own curve, then the profile's primaries are
// converted to Rec.709 (colours outside Rec.709 go negative, as in RAW decoding).
// rowCodes(y, scratch) gives row y's RGBA code values (T is uint8_t or uint16_t); it may fill and
// return scratch, which holds a row.
template <typename T, typename RowCodes>
void decodeRows(Image& img, int levels, bool srgbToLinear, const icc::Profile* profile, RowCodes&& rowCodes) {
    const std::vector<float> alpha = decodeTable(levels, false);
    const size_t n = size_t(img.w) * 4;
    if (!profile) {
        const std::vector<float> rgb = decodeTable(levels, srgbToLinear);
        parallelFor(img.h, [&](int y) {
            thread_local std::vector<T> scratch;
            scratch.resize(n);
            const T* data = rowCodes(y, scratch.data());
            float* d = img.pixel(size_t(y) * img.w);
            for (size_t i = 0; i < n; ++i) d[i] = ((i & 3) == 3 ? alpha : rgb)[data[i]];
        });
        return;
    }
    const std::vector<float> t[3] = {decodeTable(levels, true, &profile->trc[0]),
                                     decodeTable(levels, true, &profile->trc[1]),
                                     decodeTable(levels, true, &profile->trc[2])};
    const auto& m = profile->toRec709;
    parallelFor(img.h, [&](int y) {
        thread_local std::vector<T> scratch;
        scratch.resize(n);
        const T* data = rowCodes(y, scratch.data());
        float* row = img.pixel(size_t(y) * img.w);
        for (size_t i = 0; i < n; i += 4) {
            const float r = t[0][data[i]], g = t[1][data[i + 1]], b = t[2][data[i + 2]];
            float* d = row + i;
            d[0] = m[0][0] * r + m[0][1] * g + m[0][2] * b;
            d[1] = m[1][0] * r + m[1][1] * g + m[1][2] * b;
            d[2] = m[2][0] * r + m[2][1] * g + m[2][2] * b;
            d[3] = alpha[data[i + 3]];
        }
    });
}

// A whole decoded RGBA buffer (stb's output).
template <typename T>
void decodePixels(const T* data, Image& img, int levels, bool srgbToLinear, const icc::Profile* profile) {
    decodeRows<T>(img, levels, srgbToLinear, profile, [&](int y, T*) { return data + size_t(y) * img.w * 4; });
}

// The embedded profile to decode with: null for none, sRGB ones, and ones that can't be applied.
// info receives what embeddedProfileInfo reports.
std::unique_ptr<icc::Profile> usableProfile(const std::string& pathU8, std::string* info = nullptr) {
    const std::vector<uint8_t> bytes = icc::embeddedProfile(pathU8);
    if (bytes.empty()) return nullptr;
    auto p = std::make_unique<icc::Profile>();
    std::string why;
    if (!icc::parse(bytes, *p, &why)) {
        if (info) *info = "unsupported profile (" + why + "), read as sRGB";
        return nullptr;
    }
    if (icc::isSrgb(*p)) return nullptr;
    if (info) *info = p->name.empty() ? std::string("embedded profile") : p->name;
    return p;
}

std::string lowerExt(const std::string& pathU8) {
    std::string e = pathToU8(u8ToPath(pathU8).extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

// What a file says about its pixels beyond their values.
struct FileMeta {
    bool linearData = false;  // float samples (OpenEXR, float TIFF): scene-linear light
    int orientation = 1;      // TIFF orientation tag (JPEGs read theirs from EXIF)
};

std::shared_ptr<Image> loadStb(const std::string& pathU8, std::string& err, bool srgbToLinear,
                               const icc::Profile* profile, FileMeta& meta) {
    // One read of the file serves every decoder.
    std::vector<char> file;
    if (!readFileBytes(pathU8, file)) {
        err = "can't open file";
        return nullptr;
    }
    const auto* bytes = reinterpret_cast<const stbi_uc*>(file.data());
    if (tiffdec::isTiff(bytes, file.size())) {
        tiffdec::Decoded d;
        if (!tiffdec::decode(bytes, file.size(), d, err)) return nullptr;
        auto img = std::make_shared<Image>(d.info.w, d.info.h);
        meta.orientation = d.info.orientation;
        if (d.info.isFloat) {
            std::copy(d.floats.begin(), d.floats.end(), img->px.begin());
            meta.linearData = true;
        } else {
            decodePixels<uint16_t>(d.codes.data(), *img, d.maxCode + 1, srgbToLinear, profile);
        }
        return img;
    }
    if (exrdec::isExr(bytes, file.size())) {
        int w = 0, h = 0;
        std::vector<float> rgba;
        if (!exrdec::decode(bytes, file.size(), w, h, rgba, err)) return nullptr;
        auto img = std::make_shared<Image>(w, h);
        std::copy(rgba.begin(), rgba.end(), img->px.begin());
        meta.linearData = true;
        return img;
    }
    if (png::Decoded d; png::decode(bytes, file.size(), d)) {
        // Straight from the unfiltered rows to float, one row at a time.
        auto img = std::make_shared<Image>(d.w, d.h);
        if (d.sixteen)
            decodeRows<uint16_t>(*img, 65536, srgbToLinear, profile, [&](int y, uint16_t* row) {
                d.expandRow(y, row);
                return row;
            });
        else
            decodeRows<uint8_t>(*img, 256, srgbToLinear, profile, [&](int y, uint8_t* row) {
                d.expandRow(y, row);
                return row;
            });
        return img;
    }
    if (jpeg::Decoded d; jpeg::decode(bytes, file.size(), d)) {
        auto img = std::make_shared<Image>(d.w, d.h);
        decodeRows<uint8_t>(*img, 256, srgbToLinear, profile, [&](int y, uint8_t* row) {
            d.expandRow(y, row);
            return row;
        });
        return img;
    }
    if (file.size() > size_t(INT_MAX)) {
        err = "file too large";
        return nullptr;
    }
    const int len = int(file.size());
    int w = 0, h = 0, comp = 0;
    std::shared_ptr<Image> img;
    if (stbi_is_16_bit_from_memory(bytes, len)) {
        stbi_us* data = stbi_load_16_from_memory(bytes, len, &w, &h, &comp, 4);
        if (!data) {
            err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
            return nullptr;
        }
        img = std::make_shared<Image>(w, h);
        decodePixels(data, *img, 65536, srgbToLinear, profile);
        stbi_image_free(data);
    } else {
        stbi_uc* data = stbi_load_from_memory(bytes, len, &w, &h, &comp, 4);
        if (!data) {
            err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
            return nullptr;
        }
        img = std::make_shared<Image>(w, h);
        decodePixels(data, *img, 256, srgbToLinear, profile);
        stbi_image_free(data);
    }
    return img;
}

// Float files hold scene-linear light. Scene-linear projects take it as stored (or decode it from
// the sRGB curve when Color Space says so); legacy ones get sRGB-encoded 0..1 values, as RAW files
// do. Non-finite values (OpenEXR allows them) become 0.
void finishLinearData(Image& img, const DecodeOptions& opt) {
    parallelFor(img.h, [&](int y) {
        float* p = img.pixel(size_t(y) * img.w);
        for (int i = 0; i < img.w * 4; ++i) {
            float v = std::isfinite(p[i]) ? p[i] : 0.0f;
            if ((i & 3) != 3) {
                if (!opt.sceneLinear) v = float(colormath::linearToSrgb(std::clamp(v, 0.0f, 1.0f)));
                else if (opt.srgbToLinear) v = float(colormath::srgbToLinear(v));
            }
            p[i] = v;
        }
    });
}

}  // namespace

const char* const kImageFileFilter =
    "Images|*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.exr;*.bmp;*.tga;*.cr2;*.cr3;*.crw;*.nef;*.nrw;*.arw;*.srf;*.sr2;*.dng;*.raf;*.orf;"
    "*.rw2;*.pef;*.srw;*.3fr;*.iiq;*.x3f;*.mos;*.erf;*.kdc;*.mrw;*.raw;*.rwl|All files|*.*";

bool isLinearImageFile(const std::string& pathU8) {
    const std::string e = lowerExt(pathU8);
    if (e == ".exr") return true;
    if (e != ".tif" && e != ".tiff") return false;
    tiffdec::Info info;
    std::string err;
    return tiffdec::probeFile(pathU8, info, err) && info.isFloat;
}

bool isImageFile(const std::string& pathU8) {
    const std::string e = lowerExt(pathU8);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tif" || e == ".tiff" || e == ".exr" || e == ".bmp" ||
           e == ".tga" || raw::isRawPath(pathU8);
}

std::shared_ptr<Image> loadImage(const std::string& pathU8, std::string& err, const DecodeOptions& opt, bool preview,
                                 int* fullW, int* fullH) {
    if (raw::isRawPath(pathU8)) {
        auto img = raw::load(pathU8, err, opt.rawHighlights, preview, fullW, fullH);
        if (img && !opt.sceneLinear) {
            // Legacy projects work on display-encoded values, so give them what a camera JPEG
            // would hold: sRGB-encoded and clipped to 0..1.
            parallelFor(img->h, [&](int y) {
                float* p = img->pixel(size_t(y) * img->w);
                for (int i = 0; i < img->w * 4; ++i)
                    if ((i & 3) != 3) p[i] = float(colormath::linearToSrgb(std::clamp(p[i], 0.0f, 1.0f)));
            });
        }
        return img;
    }
    std::unique_ptr<icc::Profile> profile;
    if (opt.srgbToLinear && opt.embeddedProfile) profile = usableProfile(pathU8);
    FileMeta meta;
    auto img = loadStb(pathU8, err, opt.srgbToLinear, profile.get(), meta);
    if (img && meta.linearData) finishLinearData(*img, opt);
    if (img && opt.sceneLinear) {
        const std::string e = lowerExt(pathU8);
        if (e == ".jpg" || e == ".jpeg") img = exif::applyOrientation(img, exif::jpegOrientation(pathU8));
        else if (meta.orientation != 1) img = exif::applyOrientation(img, meta.orientation);
    }
    if (img && fullW) *fullW = img->w;
    if (img && fullH) *fullH = img->h;
    return img;
}

std::string embeddedProfileInfo(const std::string& pathU8) {
    // Asked every frame by the Inspector; the file is read once.
    static std::mutex mutex;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(pathU8); it != cache.end()) return it->second;
    }
    std::string info;
    if (!raw::isRawPath(pathU8)) usableProfile(pathU8, &info);
    std::lock_guard lock(mutex);
    return cache[pathU8] = info;
}

bool saveImage(const std::string& pathU8, const Image& img, std::string& err, int jpegQuality) {
    if (img.empty()) {
        err = "nothing to save";
        return false;
    }
    std::string ext = pathToU8(u8ToPath(pathU8).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const bool jpeg = ext == ".jpg" || ext == ".jpeg";
    // Drop alpha when it is fully opaque: a quarter less data to compress, and JPEG ignores it anyway.
    bool opaque = true;
    for (size_t i = 0, n = img.pixelCount(); i < n && opaque; ++i)
        if (img.px[i * 4 + 3] < 1.0f) opaque = false;
    const int comp = (jpeg || opaque) ? 3 : 4;
    std::vector<unsigned char> bytes(img.pixelCount() * comp);
    parallelFor(img.h, [&](int y) {
        for (int x = 0; x < img.w; ++x) {
            const size_t i = size_t(y) * img.w + x;
            for (int c = 0; c < comp; ++c) {
                float v = std::clamp(img.px[i * 4 + c], 0.0f, 1.0f);
                bytes[i * comp + c] = static_cast<unsigned char>(std::lround(v * 255.0f));
            }
        }
    });
    int ok = 0;
    if (jpeg) {
        ok = stbi_write_jpg(pathU8.c_str(), img.w, img.h, comp, bytes.data(), std::clamp(jpegQuality, 1, 100));
    } else {
        // stb's default level 8 spends most of the export time searching for matches for a few
        // percent smaller files; 4 is several times faster.
        stbi_write_png_compression_level = 4;
        ok = stbi_write_png(pathU8.c_str(), img.w, img.h, comp, bytes.data(), img.w * comp);
    }
    if (!ok) err = "could not write file";
    return ok != 0;
}

std::shared_ptr<Image> decodeImageMemory(const unsigned char* data, size_t len, std::string& err) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(data, int(len), &w, &h, &n, 4);
    if (!px) {
        err = stbi_failure_reason() ? stbi_failure_reason() : "can't decode image";
        return nullptr;
    }
    auto img = std::make_shared<Image>(w, h);
    for (size_t i = 0; i < size_t(w) * h * 4; ++i) img->px[i] = px[i] / 255.0f;
    stbi_image_free(px);
    return img;
}

std::vector<unsigned char> encodeJpegMemory(const Image& img, int quality) {
    std::vector<unsigned char> rgb(size_t(img.w) * img.h * 3), out;
    for (size_t i = 0; i < size_t(img.w) * img.h; ++i)
        for (int c = 0; c < 3; ++c)
            rgb[i * 3 + c] = static_cast<unsigned char>(std::lround(std::clamp(img.px[i * 4 + c], 0.0f, 1.0f) * 255.0f));
    auto append = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        v->insert(v->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
    };
    if (img.w <= 0 || img.h <= 0 || !stbi_write_jpg_to_func(append, &out, img.w, img.h, 3, rgb.data(), std::clamp(quality, 1, 100)))
        out.clear();
    return out;
}

std::shared_ptr<const Image> downscaleToFit(const std::shared_ptr<const Image>& src, int maxEdge) {
    if (!src || std::max(src->w, src->h) <= maxEdge) return src;
    const double scale = double(maxEdge) / std::max(src->w, src->h);
    const int w = std::max(1, int(std::lround(src->w * scale)));
    const int h = std::max(1, int(std::lround(src->h * scale)));
    auto out = std::make_shared<Image>(w, h);
    const double fx = double(src->w) / w, fy = double(src->h) / h;
    parallelFor(h, [&](int y) {
        int y0 = int(y * fy), y1 = std::max(y0 + 1, int((y + 1) * fy));
        y1 = std::min(y1, src->h);
        for (int x = 0; x < w; ++x) {
            int x0 = int(x * fx), x1 = std::max(x0 + 1, int((x + 1) * fx));
            x1 = std::min(x1, src->w);
            float acc[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const float* s = src->pixel(size_t(sy) * src->w + sx);
                    for (int c = 0; c < 4; ++c) acc[c] += s[c];
                }
            float inv = 1.0f / float((y1 - y0) * (x1 - x0));
            float* d = out->pixel(size_t(y) * w + x);
            for (int c = 0; c < 4; ++c) d[c] = acc[c] * inv;
        }
    });
    return out;
}
