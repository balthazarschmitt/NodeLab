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

#include "core/Parallel.h"
#include "io/Paths.h"

std::shared_ptr<Image> loadImage(const std::string& pathU8, std::string& err) {
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
        const size_t n = img->px.size();
        for (size_t i = 0; i < n; ++i) img->px[i] = data[i] / 65535.0f;
        stbi_image_free(data);
    } else {
        stbi_uc* data = stbi_load(p, &w, &h, &comp, 4);
        if (!data) {
            err = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
            return nullptr;
        }
        img = std::make_shared<Image>(w, h);
        const size_t n = img->px.size();
        for (size_t i = 0; i < n; ++i) img->px[i] = data[i] / 255.0f;
        stbi_image_free(data);
    }
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
