#include "io/ImageWrite.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <stb_image_write.h>
#include <zlib.h>

#include "core/Parallel.h"
#include "io/Paths.h"
#include "io/Tiff.h"

namespace {

using Bytes = std::vector<uint8_t>;

void be16(Bytes& o, uint32_t v) {
    o.push_back(uint8_t(v >> 8));
    o.push_back(uint8_t(v));
}
void be32(Bytes& o, uint32_t v) {
    be16(o, v >> 16);
    be16(o, v & 0xFFFF);
}

// zlib level for every format: 6 (zlib's default) packs 16-bit photos about 15% smaller than
// level 1, and the parallel compression keeps it fast.
constexpr int kZlibLevel = 6;

// One zlib stream (the format of TIFF Deflate strips and OpenEXR ZIP blocks), for blocks that
// are compressed in parallel with each other.
Bytes zlibBlock(const uint8_t* data, size_t n) {
    uLongf len = compressBound(uLong(n));
    Bytes out(len);
    if (compress2(out.data(), &len, data, uLong(n), kZlibLevel) != Z_OK) throw std::runtime_error("zlib failed");
    out.resize(len);
    return out;
}

// One zlib stream for a large buffer (PNG's image data), compressed in parallel the way pigz
// does: 1 MB segments, each primed with the 32 KB before it as its dictionary and ended with a
// sync flush (the last one ends the stream), joined under one header and Adler-32 checksum.
Bytes zlibParallel(const uint8_t* data, size_t n) {
    constexpr size_t kSeg = size_t(1) << 20, kDict = 32768;
    const int segs = int(std::max<size_t>(1, (n + kSeg - 1) / kSeg));
    std::vector<Bytes> parts(segs);
    parallelFor(segs, [&](int i) {
        const size_t at = size_t(i) * kSeg, len = std::min(kSeg, n - at);
        z_stream z{};
        if (deflateInit2(&z, kZlibLevel, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw std::runtime_error("zlib failed");
        if (at > 0) {
            const size_t d = std::min(kDict, at);
            deflateSetDictionary(&z, data + at - d, uInt(d));
        }
        Bytes& out = parts[i];
        out.resize(deflateBound(&z, uLong(len)) + 16);  // + the sync flush marker
        z.next_in = const_cast<Bytef*>(data + at);
        z.avail_in = uInt(len);
        z.next_out = out.data();
        z.avail_out = uInt(out.size());
        const int r = deflate(&z, i + 1 == segs ? Z_FINISH : Z_SYNC_FLUSH);
        out.resize(out.size() - z.avail_out);
        deflateEnd(&z);
        if (r != (i + 1 == segs ? Z_STREAM_END : Z_OK) || z.avail_in) throw std::runtime_error("zlib failed");
    });
    Bytes out = {0x78, 0x9C};  // deflate, 32 KB window, default level
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    be32(out, uint32_t(adler32_z(adler32(0, nullptr, 0), data, n)));
    return out;
}

bool writeFile(const std::string& pathU8, const Bytes& data, std::string& err) {
    std::ofstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
        err = "could not write file";
        return false;
    }
    return true;
}

bool opaque(const Image& img) {
    for (size_t i = 0, n = img.pixelCount(); i < n; ++i)
        if (img.px[i * 4 + 3] < 1.0f) return false;
    return true;
}

// Display values 0..1 to integers 0..maxV, `comp` channels per pixel, row-major.
template <class T>
std::vector<T> quantise(const Image& img, int comp, float maxV) {
    std::vector<T> out(img.pixelCount() * comp);
    parallelFor(img.h, [&](int y) {
        for (int x = 0; x < img.w; ++x) {
            const size_t i = size_t(y) * img.w + x;
            for (int c = 0; c < comp; ++c) {
                const float v = img.px[i * 4 + c];
                out[i * comp + c] = T(std::lround(std::clamp(v == v ? v : 0.0f, 0.0f, 1.0f) * maxV));
            }
        }
    });
    return out;
}

// ---------------------------------------------------------------- PNG

Bytes pngChunk(const char* type, const Bytes& data) {
    Bytes c;
    be32(c, uint32_t(data.size()));
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    be32(c, uint32_t(crc32_z(crc32(0, nullptr, 0), c.data() + 4, c.size() - 4)));
    return c;
}

// Image data as IDAT chunks of at most 1 MB: one huge chunk is valid but some readers (OpenCV)
// refuse chunks that large, and libpng itself writes small ones.
Bytes idatChunks(const uint8_t* z, size_t n) {
    constexpr size_t kMax = size_t(1) << 20;
    Bytes out;
    size_t at = 0;
    do {
        const Bytes c = pngChunk("IDAT", Bytes(z + at, z + std::min(n, at + kMax)));
        out.insert(out.end(), c.begin(), c.end());
        at += kMax;
    } while (at < n);
    return out;
}

// The sRGB chunk (perceptual intent) plus the gAMA fallback the PNG spec recommends with it.
Bytes pngSrgbChunks() {
    Bytes out = pngChunk("sRGB", {0});
    Bytes g;
    be32(g, 45455);
    Bytes ga = pngChunk("gAMA", g);
    out.insert(out.end(), ga.begin(), ga.end());
    return out;
}

// 8- or 16-bit PNG: rows filtered in parallel (per row, the filter with the smallest sum of
// absolute differences, the heuristic libpng and stb use), then compressed in parallel.
Bytes encodePng(const Image& img, int comp, int depth, bool srgb) {
    const bool sixteen = depth >= 16;
    std::vector<uint16_t> px16;
    std::vector<uint8_t> px8;
    if (sixteen) px16 = quantise<uint16_t>(img, comp, 65535.0f);
    else px8 = quantise<uint8_t>(img, comp, 255.0f);
    const int bpp = comp * (sixteen ? 2 : 1);
    const size_t rowBytes = size_t(img.w) * bpp;
    Bytes filtered(size_t(img.h) * (rowBytes + 1));
    parallelFor(img.h, [&](int y) {
        Bytes cur(rowBytes), prev(rowBytes, 0), trial(rowBytes);
        auto load = [&](Bytes& dst, int row) {
            const size_t n = size_t(img.w) * comp;
            if (!sixteen) {
                std::memcpy(dst.data(), px8.data() + size_t(row) * n, n);
                return;
            }
            const uint16_t* s = px16.data() + size_t(row) * n;
            for (size_t i = 0; i < n; ++i) {
                dst[i * 2] = uint8_t(s[i] >> 8);  // PNG samples are big-endian
                dst[i * 2 + 1] = uint8_t(s[i]);
            }
        };
        load(cur, y);
        if (y > 0) load(prev, y - 1);
        uint8_t* outRow = filtered.data() + size_t(y) * (rowBytes + 1);
        long best = -1;
        for (int f = 0; f < 5; ++f) {
            long sum = 0;
            for (size_t i = 0; i < rowBytes; ++i) {
                const int a = i >= size_t(bpp) ? cur[i - bpp] : 0, b = prev[i], c = i >= size_t(bpp) ? prev[i - bpp] : 0;
                int pred = 0;
                switch (f) {
                    case 1: pred = a; break;
                    case 2: pred = b; break;
                    case 3: pred = (a + b) >> 1; break;
                    case 4: {
                        const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                        pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                        break;
                    }
                }
                trial[i] = uint8_t(cur[i] - pred);
                sum += std::abs(int(int8_t(trial[i])));
            }
            if (best < 0 || sum < best) {
                best = sum;
                outRow[0] = uint8_t(f);
                std::memcpy(outRow + 1, trial.data(), rowBytes);
            }
        }
    });
    Bytes png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes ihdr;
    be32(ihdr, uint32_t(img.w));
    be32(ihdr, uint32_t(img.h));
    // Bit depth, colour type RGB(A), deflate, adaptive filtering, no interlace.
    ihdr.insert(ihdr.end(), {uint8_t(sixteen ? 16 : 8), uint8_t(comp == 4 ? 6 : 2), 0, 0, 0});
    Bytes c = pngChunk("IHDR", ihdr);
    png.insert(png.end(), c.begin(), c.end());
    if (srgb) {
        Bytes s = pngSrgbChunks();
        png.insert(png.end(), s.begin(), s.end());
    }
    const Bytes z = zlibParallel(filtered.data(), filtered.size());
    c = idatChunks(z.data(), z.size());
    png.insert(png.end(), c.begin(), c.end());
    c = pngChunk("IEND", {});
    png.insert(png.end(), c.begin(), c.end());
    return png;
}

void appendBytes(void* ctx, void* data, int size) {
    auto* out = static_cast<Bytes*>(ctx);
    out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

bool writePng(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err) {
    return writeFile(pathU8, encodePng(img, opaque(img) ? 3 : 4, opt.depth, opt.srgb), err);
}

// ---------------------------------------------------------------- JPEG

bool writeJpeg(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err) {
    const std::vector<uint8_t> bytes = quantise<uint8_t>(img, 3, 255.0f);
    Bytes jpg;
    if (!stbi_write_jpg_to_func(appendBytes, &jpg, img.w, img.h, 3, bytes.data(), std::clamp(opt.jpegQuality, 1, 100)) ||
        jpg.size() < 4) {
        err = "could not encode JPEG";
        return false;
    }
    // Metadata segments go after SOI and stb's JFIF APP0, the order libjpeg and exiftool write.
    size_t at = 2;
    if (jpg[2] == 0xFF && jpg[3] == 0xE0) at = 4 + (size_t(jpg[4]) << 8 | jpg[5]);
    Bytes segs;
    auto segment = [&](uint8_t marker, const char* id, size_t idLen, const Bytes& payload) {
        const size_t len = 2 + idLen + payload.size();
        if (len > 0xFFFF) return;  // too big for one segment: skip rather than corrupt the file
        segs.insert(segs.end(), {0xFF, marker, uint8_t(len >> 8), uint8_t(len)});
        segs.insert(segs.end(), id, id + idLen);
        segs.insert(segs.end(), payload.begin(), payload.end());
    };
    if (!opt.exif.empty()) segment(0xE1, "Exif\0\0", 6, opt.exif);
    if (opt.srgb) {
        Bytes icc = {1, 1};  // chunk 1 of 1
        icc.insert(icc.end(), srgbIccProfile().begin(), srgbIccProfile().end());
        segment(0xE2, "ICC_PROFILE\0", 12, icc);
    }
    jpg.insert(jpg.begin() + at, segs.begin(), segs.end());
    return writeFile(pathU8, jpg, err);
}

// ---------------------------------------------------------------- TIFF

// Baseline TIFF, Adobe Deflate with the horizontal predictor (what Photoshop and Lightroom write
// for "ZIP"), in strips compressed in parallel.
bool writeTiff(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err) {
    const int comp = opaque(img) ? 3 : 4;
    const bool sixteen = opt.depth >= 16;
    const int bytesPer = sixteen ? 2 : 1;
    const size_t rowBytes = size_t(img.w) * comp * bytesPer;
    const int rowsPerStrip = int(std::clamp<size_t>(262144 / std::max<size_t>(rowBytes, 1), 1, size_t(img.h)));
    const int strips = (img.h + rowsPerStrip - 1) / rowsPerStrip;

    std::vector<uint16_t> px16;
    std::vector<uint8_t> px8;
    if (sixteen) px16 = quantise<uint16_t>(img, comp, 65535.0f);
    else px8 = quantise<uint8_t>(img, comp, 255.0f);

    std::vector<Bytes> packed(strips);
    parallelFor(strips, [&](int s) {
        const int y0 = s * rowsPerStrip, y1 = std::min(img.h, y0 + rowsPerStrip);
        Bytes raw(size_t(y1 - y0) * rowBytes);
        const size_t n = size_t(img.w) * comp;  // samples per row
        for (int y = y0; y < y1; ++y) {
            uint8_t* dst = raw.data() + size_t(y - y0) * rowBytes;
            if (sixteen) {
                const uint16_t* src = px16.data() + size_t(y) * n;
                for (size_t i = 0; i < n; ++i) {
                    // Predictor 2: each sample minus the same channel of the pixel to its left.
                    const uint16_t v = uint16_t(src[i] - (i >= size_t(comp) ? src[i - comp] : 0));
                    dst[i * 2] = uint8_t(v);  // little-endian ("II")
                    dst[i * 2 + 1] = uint8_t(v >> 8);
                }
            } else {
                const uint8_t* src = px8.data() + size_t(y) * n;
                for (size_t i = 0; i < n; ++i) dst[i] = uint8_t(src[i] - (i >= size_t(comp) ? src[i - comp] : 0));
            }
        }
        packed[s] = zlibBlock(raw.data(), raw.size());
    });

    Bytes out = tiff::header();
    std::vector<uint32_t> offsets, counts;
    for (const Bytes& p : packed) {
        offsets.push_back(uint32_t(out.size()));
        counts.push_back(uint32_t(p.size()));
        out.insert(out.end(), p.begin(), p.end());
    }
    if (out.size() > 0xFFFFFFF0u) {
        err = "image too large for TIFF";
        return false;
    }
    tiff::Ifd ifd;
    ifd.longs(256, {uint32_t(img.w)});
    ifd.longs(257, {uint32_t(img.h)});
    ifd.shorts(258, std::vector<uint32_t>(comp, sixteen ? 16 : 8));  // BitsPerSample
    ifd.shorts(259, {8});                                            // Compression: Adobe Deflate
    ifd.shorts(262, {2});                                            // Photometric: RGB
    ifd.longs(273, offsets);                                         // StripOffsets
    ifd.shorts(274, {1});                                            // Orientation: top-left
    ifd.shorts(277, {uint32_t(comp)});                               // SamplesPerPixel
    ifd.longs(278, {uint32_t(rowsPerStrip)});
    ifd.longs(279, counts);  // StripByteCounts
    ifd.rational(282, 72, 1);
    ifd.rational(283, 72, 1);
    ifd.shorts(284, {1});  // PlanarConfiguration: chunky
    ifd.shorts(296, {2});  // ResolutionUnit: inch
    ifd.ascii(305, "NodeLab");
    ifd.shorts(317, {2});                                 // Predictor: horizontal differencing
    if (comp == 4) ifd.shorts(338, {2});                  // ExtraSamples: unassociated alpha
    ifd.shorts(339, std::vector<uint32_t>(comp, 1));      // SampleFormat: unsigned integer
    if (opt.srgb) ifd.undefined(34675, srgbIccProfile());  // InterColorProfile
    tiff::set32(out, 4, ifd.write(out));
    return writeFile(pathU8, out, err);
}

// ---------------------------------------------------------------- OpenEXR

void attr(Bytes& o, const char* name, const char* type, const Bytes& value) {
    o.insert(o.end(), name, name + std::strlen(name) + 1);
    o.insert(o.end(), type, type + std::strlen(type) + 1);
    tiff::put32(o, uint32_t(value.size()));  // EXR is little-endian too
    o.insert(o.end(), value.begin(), value.end());
}

Bytes le32s(std::initializer_list<uint32_t> v) {
    Bytes b;
    for (uint32_t x : v) tiff::put32(b, x);
    return b;
}

Bytes lef(std::initializer_list<float> v) {
    Bytes b;
    for (float f : v) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        tiff::put32(b, u);
    }
    return b;
}

// Scanline OpenEXR with ZIP compression (16-line blocks), compressed in parallel.
bool writeExr(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err) {
    const bool alpha = !opaque(img);
    const bool half = opt.depth < 32;
    const int sampleBytes = half ? 2 : 4;
    // Channels are stored in alphabetical order.
    std::vector<char> chans = alpha ? std::vector<char>{'A', 'B', 'G', 'R'} : std::vector<char>{'B', 'G', 'R'};
    auto chanIndex = [](char c) { return c == 'R' ? 0 : c == 'G' ? 1 : c == 'B' ? 2 : 3; };

    Bytes out = {0x76, 0x2F, 0x31, 0x01, 2, 0, 0, 0};  // magic, version 2, single-part scanline
    Bytes chlist;
    for (char c : chans) {
        chlist.insert(chlist.end(), {uint8_t(c), 0});
        tiff::put32(chlist, half ? 1 : 2);   // HALF or FLOAT
        chlist.insert(chlist.end(), {0, 0, 0, 0});  // pLinear, reserved
        tiff::put32(chlist, 1);
        tiff::put32(chlist, 1);  // x/y sampling
    }
    chlist.push_back(0);
    attr(out, "channels", "chlist", chlist);
    attr(out, "compression", "compression", {3});  // ZIP_COMPRESSION
    const Bytes box = le32s({0, 0, uint32_t(img.w - 1), uint32_t(img.h - 1)});
    attr(out, "dataWindow", "box2i", box);
    attr(out, "displayWindow", "box2i", box);
    attr(out, "lineOrder", "lineOrder", {0});  // INCREASING_Y
    attr(out, "pixelAspectRatio", "float", lef({1.0f}));
    attr(out, "screenWindowCenter", "v2f", lef({0.0f, 0.0f}));
    attr(out, "screenWindowWidth", "float", lef({1.0f}));
    out.push_back(0);  // end of header

    constexpr int kLines = 16;
    const int blocks = (img.h + kLines - 1) / kLines;
    std::vector<Bytes> packed(blocks);
    parallelFor(blocks, [&](int b) {
        const int y0 = b * kLines, y1 = std::min(img.h, y0 + kLines);
        Bytes raw;
        raw.reserve(size_t(y1 - y0) * img.w * chans.size() * sampleBytes);
        for (int y = y0; y < y1; ++y)
            for (char c : chans) {
                const int ci = chanIndex(c);
                for (int x = 0; x < img.w; ++x) {
                    const float* p = img.pixel(size_t(y) * img.w + x);
                    float v = p[ci];
                    // Colour premultiplied by alpha, as OpenEXR (and Blender) expect.
                    if (alpha && ci < 3) v *= p[3];
                    if (v != v) v = 0.0f;
                    if (half) {
                        tiff::put16(raw, floatToHalf(v));
                    } else {
                        uint32_t u;
                        std::memcpy(&u, &v, 4);
                        tiff::put32(raw, u);
                    }
                }
            }
        // ZIP's preprocessing: split even and odd bytes, then delta-encode, so zlib sees the
        // slowly varying high bytes together.
        Bytes t(raw.size());
        const size_t halfN = (raw.size() + 1) / 2;
        for (size_t i = 0; i < raw.size(); ++i) t[(i & 1) ? halfN + i / 2 : i / 2] = raw[i];
        for (size_t i = t.size() - 1; i > 0; --i) t[i] = uint8_t(int(t[i]) - int(t[i - 1]) + 128);
        Bytes z = zlibBlock(t.data(), t.size());
        // A block that doesn't shrink is stored raw; readers tell by its size.
        Bytes& chunk = packed[b];
        tiff::put32(chunk, uint32_t(y0));
        const Bytes& data = z.size() < raw.size() ? z : raw;
        tiff::put32(chunk, uint32_t(data.size()));
        chunk.insert(chunk.end(), data.begin(), data.end());
    });

    // Offset table (one uint64 per block), then the blocks.
    uint64_t pos = out.size() + size_t(blocks) * 8;
    for (const Bytes& p : packed) {
        tiff::put32(out, uint32_t(pos));
        tiff::put32(out, uint32_t(pos >> 32));
        pos += p.size();
    }
    for (const Bytes& p : packed) out.insert(out.end(), p.begin(), p.end());
    return writeFile(pathU8, out, err);
}

}  // namespace

const char* const kSaveImageFilter = "PNG image|*.png|JPEG image|*.jpg|TIFF image|*.tif|OpenEXR image|*.exr";

const char* formatExtension(FileFormat f) {
    switch (f) {
        case FileFormat::JPEG: return ".jpg";
        case FileFormat::TIFF: return ".tif";
        case FileFormat::EXR: return ".exr";
        default: return ".png";
    }
}

FileFormat formatFromPath(const std::string& pathU8) {
    std::string e = pathToU8(u8ToPath(pathU8).extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (e == ".jpg" || e == ".jpeg") return FileFormat::JPEG;
    if (e == ".tif" || e == ".tiff") return FileFormat::TIFF;
    if (e == ".exr") return FileFormat::EXR;
    return FileFormat::PNG;
}

int formatDepth(FileFormat f, int depth) {
    switch (f) {
        case FileFormat::JPEG: return 8;
        case FileFormat::EXR: return depth >= 32 ? 32 : 16;
        default: return depth >= 16 ? 16 : 8;
    }
}

bool writeImage(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err) {
    if (img.empty()) {
        err = "nothing to save";
        return false;
    }
    SaveOptions o = opt;
    o.depth = formatDepth(o.format, o.depth);
    switch (o.format) {
        case FileFormat::JPEG: return writeJpeg(pathU8, img, o, err);
        case FileFormat::TIFF: return writeTiff(pathU8, img, o, err);
        case FileFormat::EXR: return writeExr(pathU8, img, o, err);
        default: return writePng(pathU8, img, o, err);
    }
}

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const uint32_t a = x & 0x7FFFFFFF;
    if (a > 0x7F800000) return uint16_t(sign | 0x7E00);  // NaN
    if (a >= 0x477FF000) return uint16_t(sign | 0x7BFF);  // rounds past 65504 (or inf): clamp
    if (a < 0x38800000) {                                 // below the smallest normal half
        if (a < 0x33000000) return uint16_t(sign);        // under half the smallest subnormal
        const uint32_t m = (a & 0x7FFFFF) | 0x800000;
        const int shift = 126 - int(a >> 23);
        uint32_t r = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (r & 1))) ++r;
        return uint16_t(sign | r);
    }
    uint32_t h = (a - 0x38000000) >> 13;  // rebias the exponent (127 -> 15), keep 10 mantissa bits
    const uint32_t rem = a & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) ++h;
    return uint16_t(sign | h);
}

const std::vector<uint8_t>& srgbIccProfile() {
    static const Bytes profile = [] {
        auto s15 = [](Bytes& o, double v) { be32(o, uint32_t(int32_t(std::lround(v * 65536.0)))); };
        auto xyz = [&](double X, double Y, double Z) {
            Bytes t = {'X', 'Y', 'Z', ' ', 0, 0, 0, 0};
            s15(t, X);
            s15(t, Y);
            s15(t, Z);
            return t;
        };
        Bytes desc = {'d', 'e', 's', 'c', 0, 0, 0, 0};
        const std::string name = "sRGB IEC61966-2.1";
        be32(desc, uint32_t(name.size() + 1));
        desc.insert(desc.end(), name.begin(), name.end());
        desc.push_back(0);
        desc.resize(desc.size() + 4 + 4 + 2 + 1 + 67, 0);  // empty Unicode and ScriptCode names
        Bytes cprt = {'t', 'e', 'x', 't', 0, 0, 0, 0};
        const std::string c = "No copyright, use freely";
        cprt.insert(cprt.end(), c.begin(), c.end());
        cprt.push_back(0);
        Bytes curv = {'c', 'u', 'r', 'v', 0, 0, 0, 0};
        constexpr int kN = 1024;
        be32(curv, kN);
        for (int i = 0; i < kN; ++i) {
            const double v = double(i) / (kN - 1);
            const double lin = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
            be16(curv, uint32_t(std::lround(lin * 65535.0)));
        }
        // Rec.709 primaries adapted to the D50 profile connection space with Bradford.
        struct Tag {
            const char* sig;
            Bytes data;
        };
        std::vector<Tag> tags = {
            {"desc", desc},
            {"cprt", cprt},
            {"wtpt", xyz(0.9642, 1.0, 0.8249)},
            {"rXYZ", xyz(0.4360747, 0.2225045, 0.0139322)},
            {"gXYZ", xyz(0.3850649, 0.7168786, 0.0971045)},
            {"bXYZ", xyz(0.1430804, 0.0606169, 0.7141733)},
            {"rTRC", curv},
            {"gTRC", {}},  // share rTRC's data
            {"bTRC", {}},
        };
        Bytes table, data;
        be32(table, uint32_t(tags.size()));
        const size_t dataStart = 128 + 4 + tags.size() * 12;
        uint32_t trcOff = 0, trcLen = 0;
        for (Tag& t : tags) {
            uint32_t off, len;
            if (t.data.empty()) {
                off = trcOff, len = trcLen;
            } else {
                off = uint32_t(dataStart + data.size());
                len = uint32_t(t.data.size());
                data.insert(data.end(), t.data.begin(), t.data.end());
                while (data.size() % 4) data.push_back(0);  // tags start on 4-byte boundaries
                if (std::strcmp(t.sig, "rTRC") == 0) trcOff = off, trcLen = len;
            }
            table.insert(table.end(), t.sig, t.sig + 4);
            be32(table, off);
            be32(table, len);
        }
        Bytes p;
        be32(p, uint32_t(128 + table.size() + data.size()));  // profile size
        p.insert(p.end(), 4, 0);                              // preferred CMM
        be32(p, 0x02100000);                                  // version 2.1
        for (const char* s : {"mntr", "RGB ", "XYZ "}) p.insert(p.end(), s, s + 4);
        for (uint32_t v : {2026u, 1u, 1u, 0u, 0u, 0u}) be16(p, v);  // creation date
        for (const char* s : {"acsp"}) p.insert(p.end(), s, s + 4);
        p.insert(p.end(), 4 + 4 + 4 + 4 + 8 + 4, 0);  // platform, flags, manufacturer, model, attributes, intent
        s15(p, 0.9642);                               // PCS illuminant D50
        s15(p, 1.0);
        s15(p, 0.8249);
        p.resize(128, 0);  // creator, ID, reserved
        p.insert(p.end(), table.begin(), table.end());
        p.insert(p.end(), data.begin(), data.end());
        return p;
    }();
    return profile;
}
