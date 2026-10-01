#include "io/ImageIO.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
#include "io/Paths.h"
#include "io/RawDecode.h"

namespace {

// Code value -> float for every value of an 8- or 16-bit channel, so decoding is one lookup.
std::vector<float> decodeTable(int levels, bool srgbToLinear) {
    std::vector<float> t(static_cast<size_t>(levels));
    for (int i = 0; i < levels; ++i) {
        const float v = float(i) / float(levels - 1);
        t[size_t(i)] = srgbToLinear ? float(colormath::srgbToLinear(v)) : v;
    }
    return t;
}

template <typename T>
void decodePixels(const T* data, Image& img, int levels, bool srgbToLinear) {
    const std::vector<float> rgb = decodeTable(levels, srgbToLinear), alpha = decodeTable(levels, false);
    parallelFor(img.h, [&](int y) {
        const size_t i0 = size_t(y) * img.w * 4, i1 = i0 + size_t(img.w) * 4;
        for (size_t i = i0; i < i1; ++i) img.px[i] = ((i & 3) == 3 ? alpha : rgb)[data[i]];
    });
}

std::string lowerExt(const std::string& pathU8) {
    std::string e = pathToU8(u8ToPath(pathU8).extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

std::shared_ptr<Image> loadStb(const std::string& pathU8, std::string& err, bool srgbToLinear) {
    int w = 0, h = 0, comp = 0;
    const char* p = pathU8.c_str();
    std::shared_ptr<Image> img;
    if (stbi_is_16_bit(p)) {
        stbi_us* data = stbi_load_16(p, &w, &h, &comp, 4);
        if (!data) {
            err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
            return nullptr;
        }
        img = std::make_shared<Image>(w, h);
        decodePixels(data, *img, 65536, srgbToLinear);
        stbi_image_free(data);
    } else {
        stbi_uc* data = stbi_load(p, &w, &h, &comp, 4);
        if (!data) {
            err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
            return nullptr;
        }
        img = std::make_shared<Image>(w, h);
        decodePixels(data, *img, 256, srgbToLinear);
        stbi_image_free(data);
    }
    return img;
}

}  // namespace

const char* const kImageFileFilter =
    "Images|*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.cr2;*.cr3;*.crw;*.nef;*.nrw;*.arw;*.srf;*.sr2;*.dng;*.raf;*.orf;"
    "*.rw2;*.pef;*.srw;*.3fr;*.iiq;*.x3f;*.mos;*.erf;*.kdc;*.mrw;*.raw;*.rwl|All files|*.*";

bool isImageFile(const std::string& pathU8) {
    const std::string e = lowerExt(pathU8);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga" || raw::isRawPath(pathU8);
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
    auto img = loadStb(pathU8, err, opt.srgbToLinear);
    if (img && opt.sceneLinear) {
        const std::string e = lowerExt(pathU8);
        if (e == ".jpg" || e == ".jpeg") img = exif::applyOrientation(img, exif::jpegOrientation(pathU8));
    }
    if (img && fullW) *fullW = img->w;
    if (img && fullH) *fullH = img->h;
    return img;
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
