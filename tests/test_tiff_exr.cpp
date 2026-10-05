// TIFF and OpenEXR import: our own exports read back, and hand-built TIFFs covering what other
// programs write (big-endian, LZW, PackBits, predictors, tiles, planar, palette, float, alpha).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <tuple>
#include <vector>

#include <zlib.h>

#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Paths.h"
#include "io/TiffDecode.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

Image gradient(int w, int h, bool alpha) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img.pixel(size_t(y) * w + x);
            p[0] = float(x) / float(w - 1);
            p[1] = float(y) / float(h - 1);
            p[2] = float((x * 7 + y * 3) % 17) / 16.0f;
            p[3] = alpha ? 0.25f + 0.75f * float(x) / float(w - 1) : 1.0f;
        }
    return img;
}

fs::path tempFile(const char* name) { return fs::temp_directory_path() / name; }

void writeBytes(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

double maxDiff(const Image& a, const Image& b) {
    REQUIRE(a.w == b.w);
    REQUIRE(a.h == b.h);
    double m = 0;
    for (size_t i = 0; i < a.px.size(); ++i) m = std::max(m, double(std::fabs(a.px[i] - b.px[i])));
    return m;
}

// ---- a small TIFF writer for the cases our exporter doesn't produce

struct TiffSpec {
    bool bigEndian = false;
    int bits = 8;
    bool isFloat = false;
    int photometric = 2;  // 0 WhiteIsZero, 1 grey, 2 RGB, 3 palette
    int spp = 3;
    int extra = -1;       // ExtraSamples value, -1 for no tag
    int compression = 1;  // 1, 5 LZW, 8 Deflate, 32773 PackBits
    int predictor = 1;
    int planar = 1;
    int rowsPerStrip = 3;
    int tile = 0;         // tile edge (0: strips)
};

// TIFF LZW, as libtiff writes it (including the width change after the last code).
Bytes lzwEncode(const Bytes& in) {
    Bytes out;
    uint32_t acc = 0;
    int nacc = 0, width = 9;
    auto put = [&](int code) {
        acc = acc << width | uint32_t(code);
        nacc += width;
        while (nacc >= 8) {
            out.push_back(uint8_t(acc >> (nacc - 8)));
            nacc -= 8;
        }
        acc &= (1u << nacc) - 1;
    };
    std::map<std::pair<int, int>, int> dict;
    int next = 258, w = -1;
    put(256);
    auto grow = [&] {
        ++next;
        if (next == 4094) {
            put(256);
            dict.clear(), next = 258, width = 9;
        } else if (next > (1 << width) - 1) ++width;
    };
    for (uint8_t b : in) {
        if (w < 0) {
            w = b;
            continue;
        }
        auto it = dict.find({w, b});
        if (it != dict.end()) {
            w = it->second;
            continue;
        }
        put(w);
        dict[{w, b}] = next;
        grow();
        w = b;
    }
    if (w >= 0) {
        put(w);
        grow();
    }
    put(257);
    if (nacc) out.push_back(uint8_t(acc << (8 - nacc)));
    return out;
}

Bytes packBits(const Bytes& in) {
    Bytes out;
    for (size_t i = 0; i < in.size();) {
        size_t run = 1;
        while (i + run < in.size() && run < 128 && in[i + run] == in[i]) ++run;
        if (run >= 3) {
            out.push_back(uint8_t(int8_t(1 - int(run))));
            out.push_back(in[i]);
            i += run;
            continue;
        }
        size_t lit = 0;
        while (i + lit < in.size() && lit < 128 &&
               !(i + lit + 2 < in.size() && in[i + lit] == in[i + lit + 1] && in[i + lit] == in[i + lit + 2]))
            ++lit;
        out.push_back(uint8_t(lit - 1));
        out.insert(out.end(), in.begin() + long(i), in.begin() + long(i + lit));
        i += lit;
    }
    return out;
}

uint16_t toHalf(float f) { return floatToHalf(f); }

// `samples` holds w*h*spp values (0..1 for integers, codes for palette), chunky.
Bytes makeTiff(const TiffSpec& s, int w, int h, const std::vector<float>& samples, const std::vector<uint16_t>& colormap = {}) {
    const int bytes = s.bits / 8;
    auto putN = [&](Bytes& o, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) o.push_back(uint8_t(s.bigEndian ? v >> (8 * (n - 1 - i)) : v >> (8 * i)));
    };
    // One sample in file byte order.
    auto sample = [&](Bytes& o, float v) {
        if (s.isFloat && s.bits == 32) {
            uint32_t b;
            std::memcpy(&b, &v, 4);
            putN(o, b, 4);
        } else if (s.isFloat) putN(o, toHalf(v), 2);
        else if (s.photometric == 3) putN(o, uint64_t(v), bytes);
        else putN(o, uint64_t(std::lround(v * ((1 << s.bits) - 1))), bytes);
    };
    const int cw = s.tile ? s.tile : w, chMax = s.tile ? s.tile : s.rowsPerStrip;
    const int across = (w + cw - 1) / cw, down = (h + chMax - 1) / chMax;
    const int planes = s.planar == 2 ? s.spp : 1, spc = s.planar == 2 ? 1 : s.spp;
    std::vector<Bytes> chunks;
    for (int pl = 0; pl < planes; ++pl)
        for (int ty = 0; ty < down; ++ty)
            for (int tx = 0; tx < across; ++tx) {
                const int rows = s.tile ? chMax : std::min(chMax, h - ty * chMax);
                Bytes raw;
                for (int r = 0; r < rows; ++r) {
                    Bytes row;
                    for (int x = 0; x < cw; ++x)
                        for (int c = 0; c < spc; ++c) {
                            const int gx = std::min(tx * cw + x, w - 1), gy = std::min(ty * chMax + r, h - 1);
                            sample(row, samples[(size_t(gy) * w + gx) * s.spp + size_t(s.planar == 2 ? pl : c)]);
                        }
                    if (s.predictor == 2 && bytes == 1)
                        for (size_t i = row.size(); i-- > size_t(spc);) row[i] = uint8_t(row[i] - row[i - spc]);
                    if (s.predictor == 2 && bytes == 2) {
                        std::vector<uint16_t> v(row.size() / 2);
                        for (size_t i = 0; i < v.size(); ++i)
                            v[i] = s.bigEndian ? uint16_t(row[2 * i] << 8 | row[2 * i + 1]) : uint16_t(row[2 * i + 1] << 8 | row[2 * i]);
                        for (size_t i = v.size(); i-- > size_t(spc);) v[i] = uint16_t(v[i] - v[i - spc]);
                        row.clear();
                        for (uint16_t x : v) putN(row, x, 2);
                    }
                    if (s.predictor == 3) {
                        // Bytes of each value most significant first, split into planes, then
                        // differenced along the row.
                        const size_t n = row.size() / bytes;
                        Bytes native(row.size());
                        for (size_t i = 0; i < n; ++i)
                            for (int b = 0; b < bytes; ++b)
                                native[i * bytes + b] = s.bigEndian ? row[i * bytes + b] : row[i * bytes + (bytes - 1 - b)];
                        Bytes planesB(row.size());
                        for (size_t i = 0; i < n; ++i)
                            for (int b = 0; b < bytes; ++b) planesB[size_t(b) * n + i] = native[i * bytes + b];
                        for (size_t i = planesB.size(); i-- > size_t(spc);) planesB[i] = uint8_t(planesB[i] - planesB[i - spc]);
                        row = planesB;
                    }
                    raw.insert(raw.end(), row.begin(), row.end());
                }
                if (s.compression == 5) raw = lzwEncode(raw);
                else if (s.compression == 32773) raw = packBits(raw);
                else if (s.compression == 8) {
                    uLongf n = compressBound(uLong(raw.size()));
                    Bytes z(n);
                    REQUIRE(compress2(z.data(), &n, raw.data(), uLong(raw.size()), 6) == Z_OK);
                    z.resize(n);
                    raw = z;
                }
                chunks.push_back(raw);
            }
    // Header, pixel data, then the directory and its out-of-line values.
    Bytes f;
    f.push_back(s.bigEndian ? 'M' : 'I'), f.push_back(s.bigEndian ? 'M' : 'I');
    putN(f, 42, 2);
    putN(f, 0, 4);
    std::vector<uint32_t> offsets, counts;
    for (const Bytes& c : chunks) {
        offsets.push_back(uint32_t(f.size()));
        counts.push_back(uint32_t(c.size()));
        f.insert(f.end(), c.begin(), c.end());
        if (f.size() & 1) f.push_back(0);
    }
    struct Entry {
        uint16_t tag, type;
        std::vector<uint32_t> v;
    };
    std::vector<Entry> e = {{256, 4, {uint32_t(w)}}, {257, 4, {uint32_t(h)}},
                            {258, 3, std::vector<uint32_t>(size_t(s.spp), uint32_t(s.bits))},
                            {259, 3, {uint32_t(s.compression)}}, {262, 3, {uint32_t(s.photometric)}},
                            {277, 3, {uint32_t(s.spp)}}, {284, 3, {uint32_t(s.planar)}}};
    if (s.tile) {
        e.push_back({322, 3, {uint32_t(s.tile)}});
        e.push_back({323, 3, {uint32_t(s.tile)}});
        e.push_back({324, 4, offsets});
        e.push_back({325, 4, counts});
    } else {
        e.push_back({273, 4, offsets});
        e.push_back({278, 3, {uint32_t(s.rowsPerStrip)}});
        e.push_back({279, 4, counts});
    }
    if (s.predictor != 1) e.push_back({317, 3, {uint32_t(s.predictor)}});
    if (!colormap.empty()) e.push_back({320, 3, std::vector<uint32_t>(colormap.begin(), colormap.end())});
    if (s.extra >= 0) e.push_back({338, 3, {uint32_t(s.extra)}});
    if (s.isFloat) e.push_back({339, 3, std::vector<uint32_t>(size_t(s.spp), 3u)});
    std::sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
    const uint32_t ifd = uint32_t(f.size());
    for (int i = 0; i < 4; ++i) f[4 + size_t(i)] = uint8_t(s.bigEndian ? ifd >> (8 * (3 - i)) : ifd >> (8 * i));
    Bytes values;
    const uint32_t valuesAt = ifd + 2 + uint32_t(e.size()) * 12 + 4;
    putN(f, e.size(), 2);
    for (const Entry& x : e) {
        putN(f, x.tag, 2);
        putN(f, x.type, 2);
        putN(f, x.v.size(), 4);
        const int size = x.type == 3 ? 2 : 4;
        Bytes data;
        for (uint32_t v : x.v) putN(data, v, size);
        if (data.size() <= 4) {
            data.resize(4, 0);
            f.insert(f.end(), data.begin(), data.end());
        } else {
            putN(f, valuesAt + values.size(), 4);
            values.insert(values.end(), data.begin(), data.end());
        }
    }
    putN(f, 0, 4);
    f.insert(f.end(), values.begin(), values.end());
    return f;
}

std::vector<float> samplesOf(const Image& img, int spp) {
    std::vector<float> s;
    for (size_t i = 0; i < size_t(img.w) * img.h; ++i)
        for (int c = 0; c < spp; ++c) s.push_back(img.px[i * 4 + size_t(c)]);
    return s;
}

std::shared_ptr<Image> loadBytes(const Bytes& b, const char* name, DecodeOptions opt = {}) {
    const fs::path p = tempFile(name);
    writeBytes(p, b);
    std::string err;
    auto img = loadImage(pathToU8(p), err, opt);
    fs::remove(p);
    INFO(err);
    REQUIRE(img);
    return img;
}

}  // namespace

TEST_CASE("TIFF and OpenEXR exports load back") {
    const Image src = gradient(37, 23, true);
    for (auto [format, depth, tol] : {std::tuple{FileFormat::TIFF, 8, 0.5 / 255}, std::tuple{FileFormat::TIFF, 16, 0.5 / 65535},
                                      std::tuple{FileFormat::EXR, 16, 2e-3}, std::tuple{FileFormat::EXR, 32, 1e-6}}) {
        CAPTURE(depth);
        const fs::path p = tempFile(format == FileFormat::EXR ? "nodelab_rt.exr" : "nodelab_rt.tif");
        SaveOptions o;
        o.format = format, o.depth = depth;
        std::string err;
        REQUIRE(writeImage(pathToU8(p), src, o, err));
        CHECK(isLinearImageFile(pathToU8(p)) == (format == FileFormat::EXR));
        // Scene-linear and "Linear Rec.709" (srgbToLinear off): values as stored.
        auto img = loadImage(pathToU8(p), err, DecodeOptions{false, true});
        fs::remove(p);
        INFO(err);
        REQUIRE(img);
        // OpenEXR's premultiplied colour comes back straight, within half precision.
        CHECK(maxDiff(*img, src) <= 1e-6 + tol + (format == FileFormat::EXR && depth == 16 ? 4e-3 : 0.0));
    }
}

TEST_CASE("OpenEXR keeps values above 1 and legacy projects get them sRGB-encoded") {
    Image src = gradient(16, 8, false);
    for (float& v : src.px) v *= 4.0f;
    for (size_t i = 3; i < src.px.size(); i += 4) src.px[i] = 1.0f;
    const fs::path p = tempFile("nodelab_hdr.exr");
    SaveOptions o;
    o.format = FileFormat::EXR, o.depth = 32;
    std::string err;
    REQUIRE(writeImage(pathToU8(p), src, o, err));
    auto lin = loadImage(pathToU8(p), err, DecodeOptions{false, true});
    auto legacy = loadImage(pathToU8(p), err, DecodeOptions{});
    fs::remove(p);
    REQUIRE(lin);
    REQUIRE(legacy);
    CHECK(maxDiff(*lin, src) < 1e-6);
    // Legacy: clipped to 0..1 and encoded, like a RAW in a legacy project.
    const float* q = legacy->pixel(15);  // x = 15: red 4.0 -> 1
    CHECK(q[0] == doctest::Approx(1.0f));
    const float* r = legacy->pixel(1);   // red 4/15 linear -> sRGB
    CHECK(r[0] == doctest::Approx(1.055 * std::pow(4.0 / 15.0, 1 / 2.4) - 0.055).epsilon(1e-4));
}

TEST_CASE("TIFF variants other programs write decode to the same pixels") {
    const int w = 19, h = 11;
    const Image rgb = gradient(w, h, false), rgba = gradient(w, h, true);
    auto check = [&](TiffSpec s, const Image& want, double tol, const char* what) {
        const std::string label = what;
        CAPTURE(label);
        const Bytes f = makeTiff(s, w, h, samplesOf(want, s.spp));
        tiffdec::Decoded d;
        std::string err;
        REQUIRE_MESSAGE(tiffdec::decode(f.data(), f.size(), d, err), err);
        auto img = loadBytes(f, "nodelab_variant.tif", DecodeOptions{false, true});
        Image expect = want;
        if (s.spp == 1)  // grey: the first channel everywhere, opaque
            for (size_t i = 0; i < expect.px.size(); i += 4)
                expect.px[i + 1] = expect.px[i + 2] = expect.px[i], expect.px[i + 3] = 1.0f;
        CHECK(maxDiff(*img, expect) <= tol + 1e-6);
    };
    const double t8 = 0.5 / 255, t16 = 0.5 / 65535;
    for (int comp : {1, 5, 8, 32773})
        for (bool big : {false, true}) {
            CAPTURE(comp);
            CAPTURE(big);
            TiffSpec s;
            s.compression = comp, s.bigEndian = big;
            check(s, rgb, t8, "8-bit RGB");
            s.predictor = 2;
            check(s, rgb, t8, "8-bit RGB, predictor");
            s.bits = 16;
            check(s, rgb, t16, "16-bit RGB, predictor");
            s.predictor = 1, s.spp = 4, s.extra = 2;
            check(s, rgba, t16, "16-bit RGBA");
            s.bits = 32, s.isFloat = true, s.spp = 3, s.extra = -1;
            check(s, rgb, 1e-7, "float");
            s.predictor = 3;
            check(s, rgb, 1e-7, "float, floating-point predictor");
            s.bits = 16;
            check(s, rgb, 5e-4, "half float, floating-point predictor");
            s = {};
            s.compression = comp, s.bigEndian = big, s.tile = 16;
            check(s, rgb, t8, "tiled");
            s.tile = 0, s.planar = 2, s.spp = 4, s.extra = 2;
            check(s, rgba, t8, "planar RGBA");
            s = {};
            s.compression = comp, s.bigEndian = big, s.photometric = 1, s.spp = 1, s.rowsPerStrip = 100;
            check(s, rgb, t8, "grey, one strip");
        }

    SUBCASE("premultiplied alpha comes back straight") {
        TiffSpec s;
        s.bits = 16, s.spp = 4, s.extra = 1;
        Image pre = rgba;
        for (size_t i = 0; i < pre.px.size(); i += 4)
            for (int c = 0; c < 3; ++c) pre.px[i + size_t(c)] *= pre.px[i + 3];
        auto img = loadBytes(makeTiff(s, w, h, samplesOf(pre, 4)), "nodelab_premul.tif", DecodeOptions{false, true});
        CHECK(maxDiff(*img, rgba) < 2e-4);
    }
    SUBCASE("WhiteIsZero and palette") {
        TiffSpec s;
        s.photometric = 0, s.spp = 1;
        auto img = loadBytes(makeTiff(s, w, h, samplesOf(rgb, 1)), "nodelab_wiz.tif", DecodeOptions{false, true});
        CHECK(img->px[0] == doctest::Approx(1.0f));  // stored 0 is white
        s.photometric = 3;
        std::vector<uint16_t> map(3 * 256);
        for (int i = 0; i < 256; ++i) map[size_t(i)] = uint16_t(i * 257), map[256 + size_t(i)] = 0, map[512 + size_t(i)] = 65535;
        std::vector<float> idx(size_t(w) * h, 51.0f);
        auto pal = loadBytes(makeTiff(s, w, h, idx, map), "nodelab_pal.tif", DecodeOptions{false, true});
        CHECK(pal->px[0] == doctest::Approx(0.2f));
        CHECK(pal->px[1] == 0.0f);
        CHECK(pal->px[2] == 1.0f);
    }
    SUBCASE("unsupported kinds fail with a reason") {
        TiffSpec s;
        s.photometric = 5;  // CMYK
        s.spp = 4;
        const Bytes f = makeTiff(s, w, h, samplesOf(rgba, 4));
        tiffdec::Decoded d;
        std::string err;
        CHECK_FALSE(tiffdec::decode(f.data(), f.size(), d, err));
        CHECK(err.find("CMYK") != std::string::npos);
    }
}

TEST_CASE("float TIFFs read as linear data and sRGB ones through the curve") {
    const Image rgb = gradient(8, 4, false);
    TiffSpec s;
    s.bits = 32, s.isFloat = true;
    const Bytes f = makeTiff(s, 8, 4, samplesOf(rgb, 3));
    const fs::path p = tempFile("nodelab_float.tif");
    writeBytes(p, f);
    CHECK(isLinearImageFile(pathToU8(p)));
    std::string err;
    auto asLinear = loadImage(pathToU8(p), err, DecodeOptions{false, true});
    auto asSrgb = loadImage(pathToU8(p), err, DecodeOptions{true, true});
    fs::remove(p);
    REQUIRE(asLinear);
    REQUIRE(asSrgb);
    CHECK(maxDiff(*asLinear, rgb) < 1e-7);
    CHECK(asSrgb->px[4] == doctest::Approx(float(std::pow((1.0 / 7 + 0.055) / 1.055, 2.4))).epsilon(1e-4));
}

TEST_CASE("damaged hand-built TIFFs decode or fail cleanly") {
    const Image rgba = gradient(23, 17, true);
    std::mt19937 rng(9);
    for (int comp : {5, 8, 32773}) {
        TiffSpec s;
        s.compression = comp, s.spp = 4, s.extra = 2, s.predictor = 2, s.tile = comp == 8 ? 16 : 0;
        const Bytes good = makeTiff(s, 23, 17, samplesOf(rgba, 4));
        for (int run = 0; run < 400; ++run) {
            Bytes b = good;
            for (int k = 1 + int(rng() % 6); k > 0; --k) b[4 + rng() % (b.size() - 4)] ^= uint8_t(1 + rng() % 255);
            if (rng() % 4 == 0) b.resize(8 + rng() % (b.size() - 8));
            tiffdec::Decoded d;
            std::string err;
            if (tiffdec::decode(b.data(), b.size(), d, err)) {
                CHECK(d.info.w > 0);
                CHECK(d.codes.size() + d.floats.size() == size_t(d.info.w) * d.info.h * 4);
            }
        }
    }
}
