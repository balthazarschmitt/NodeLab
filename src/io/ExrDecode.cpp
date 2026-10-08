#include "io/ExrDecode.h"

#include <cstring>
#include <new>

// tinyexr inflates ZIP blocks with zlib-ng (zlib's API) rather than its bundled miniz.
#include <zlib.h>
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 0
#define TINYEXR_IMPLEMENTATION
// GCC 16 reports a false overflow in one of tinyexr's vector fills.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include <tinyexr.h>
#pragma GCC diagnostic pop

#include "core/Parallel.h"

namespace exrdec {

bool isExr(const uint8_t* data, size_t len) {
    return len >= 4 && data[0] == 0x76 && data[1] == 0x2f && data[2] == 0x31 && data[3] == 0x01;
}

namespace {

// Frees tinyexr's structures however decoding ends.
struct Loaded {
    EXRHeader header;
    EXRImage image;
    bool headerParsed = false, imageLoaded = false;
    Loaded() {
        InitEXRHeader(&header);
        InitEXRImage(&image);
    }
    ~Loaded() {
        if (imageLoaded) FreeEXRImage(&image);
        if (headerParsed) FreeEXRHeader(&header);
    }
};

bool fail(std::string& err, const char* msg, const char* detail) {
    err = msg;
    if (detail) {
        err += std::string(" (") + detail + ")";
        FreeEXRErrorMessage(detail);
    }
    return false;
}

// The channel named `name` in layer `prefix` ("" for the top level, else "Layer." up to the
// channel name), or -1.
int findChannel(const EXRHeader& h, const std::string& prefix, const char* name) {
    for (int c = 0; c < h.num_channels; ++c)
        if (prefix + name == h.channels[c].name) return c;
    return -1;
}

}  // namespace

bool decode(const uint8_t* data, size_t len, int& w, int& h, std::vector<float>& rgba, std::string& err) {
    EXRVersion version;
    if (ParseEXRVersionFromMemory(&version, data, len) != TINYEXR_SUCCESS) return err = "damaged OpenEXR file", false;
    if (version.multipart) return err = "multi-part OpenEXR files aren't supported", false;
    if (version.non_image) return err = "deep OpenEXR files aren't supported", false;
    Loaded l;
    const char* msg = nullptr;
    if (ParseEXRHeaderFromMemory(&l.header, &version, data, len, &msg) != TINYEXR_SUCCESS)
        return fail(err, "damaged OpenEXR header", msg);
    l.headerParsed = true;
    const EXRHeader& hd = l.header;
    if (hd.num_channels <= 0) return err = "OpenEXR file has no channels", false;

    // RGB(A) at the top level, else the first layer with R, G and B; else one channel as grey
    // (Y, or whatever the file has).
    int ch[4] = {-1, -1, -1, -1};
    auto tryLayer = [&](const std::string& prefix) {
        const int r = findChannel(hd, prefix, "R"), g = findChannel(hd, prefix, "G"), b = findChannel(hd, prefix, "B");
        if (r < 0 || g < 0 || b < 0) return false;
        ch[0] = r, ch[1] = g, ch[2] = b, ch[3] = findChannel(hd, prefix, "A");
        return true;
    };
    if (!tryLayer("")) {
        bool found = false;
        for (int c = 0; c < hd.num_channels && !found; ++c) {
            const std::string name = hd.channels[c].name;
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos && name.compare(dot + 1, std::string::npos, "R") == 0)
                found = tryLayer(name.substr(0, dot + 1));
        }
        if (!found) {
            int y = findChannel(hd, "", "Y");
            ch[0] = ch[1] = ch[2] = y >= 0 ? y : 0;
            ch[3] = findChannel(hd, "", "A");
        }
    }
    // Every channel as float (half and uint convert), so the copy below reads one type.
    for (int c = 0; c < hd.num_channels; ++c) hd.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
    for (int c = 0; c < hd.num_channels; ++c)
        if (hd.pixel_types[c] == TINYEXR_PIXELTYPE_UINT) hd.requested_pixel_types[c] = TINYEXR_PIXELTYPE_UINT;
    if (LoadEXRImageFromMemory(&l.image, &hd, data, len, &msg) != TINYEXR_SUCCESS)
        return fail(err, "damaged OpenEXR image", msg);
    l.imageLoaded = true;
    const EXRImage& im = l.image;
    w = im.width, h = im.height;
    if (w <= 0 || h <= 0 || uint64_t(w) * uint64_t(h) > (uint64_t(1) << 28)) return err = "damaged OpenEXR size", false;
    try {
        rgba.assign(size_t(w) * h * 4, 0.0f);
    } catch (const std::bad_alloc&) {
        return err = "not enough memory", false;
    }
    // One pixel from a channel buffer: floats, or uints (object IDs) as numbers.
    auto value = [&](unsigned char* const* images, int c, size_t i) {
        if (hd.requested_pixel_types[c] == TINYEXR_PIXELTYPE_UINT)
            return float(reinterpret_cast<const uint32_t*>(images[c])[i]);
        return reinterpret_cast<const float*>(images[c])[i];
    };
    auto put = [&](unsigned char* const* images, size_t src, size_t dst) {
        float* p = &rgba[dst * 4];
        for (int k = 0; k < 3; ++k) p[k] = value(images, ch[k], src);
        p[3] = ch[3] >= 0 ? value(images, ch[3], src) : 1.0f;
        // OpenEXR stores colour premultiplied by alpha; Refractory's images are straight.
        if (ch[3] >= 0 && p[3] > 0.0f && p[3] != 1.0f)
            for (int k = 0; k < 3; ++k) p[k] /= p[3];
    };
    if (hd.tiled) {
        const int tw = hd.tile_size_x, th = hd.tile_size_y;
        for (int t = 0; t < im.num_tiles; ++t) {
            const EXRTile& tile = im.tiles[t];
            if (tile.level_x != 0 || tile.level_y != 0 || !tile.images) continue;
            for (int y = 0; y < tile.height; ++y)
                for (int x = 0; x < tile.width; ++x) {
                    const int64_t gx = int64_t(tile.offset_x) * tw + x, gy = int64_t(tile.offset_y) * th + y;
                    if (gx < 0 || gy < 0 || gx >= w || gy >= h) continue;
                    put(tile.images, size_t(y) * size_t(tw) + size_t(x), size_t(gy) * size_t(w) + size_t(gx));
                }
        }
    } else {
        if (!im.images) return err = "damaged OpenEXR image", false;
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) put(im.images, size_t(y) * w + x, size_t(y) * w + x);
        });
    }
    return true;
}

}  // namespace exrdec
