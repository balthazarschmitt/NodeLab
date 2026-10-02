#include "io/ImageWrite.h"

#include <algorithm>
#include <atomic>
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

// Runs fn(i) for i in [0, n) on the worker threads, one item at a time (parallelFor runs fewer
// than 16 items serially, but a handful of 1 MB zlib segments is still worth spreading).
template <class Fn>
void forEach(int n, Fn&& fn) {
    parallel::run(n, 1, [&](int b, int e) {
        for (int i = b; i < e; ++i) fn(i);
    });
}

// zlib level for every format: 6 (zlib's default) packs 16-bit photos about 15% smaller than
// level 1, and the parallel compression keeps it fast.
constexpr int kZlibLevel = 6;

// The deflate strategy for a block. A photo's filtered residuals have few long matches, so
// run-length matching alone (Z_RLE) packs them as small as the full LZ matcher, three to four
// times faster. Graphics (flat areas, repeated patterns) need the LZ matcher: there it's two to
// three times smaller. A few 16 KB slices of the block, compressed both ways, decide.
int strategyFor(const uint8_t* data, size_t n) {
    constexpr size_t kSlice = 16384;
    constexpr int kSlices = 4;
    if (n < kSlice * kSlices) return Z_DEFAULT_STRATEGY;
    size_t lz = 0, rle = 0;
    Bytes buf(compressBound(uLong(kSlice)) + 64);
    for (int s = 0; s < kSlices; ++s) {
        const uint8_t* at = data + (n - kSlice) * size_t(s) / (kSlices - 1);
        for (int strategy : {Z_DEFAULT_STRATEGY, Z_RLE}) {
            z_stream z{};
            if (deflateInit2(&z, kZlibLevel, Z_DEFLATED, -15, 8, strategy) != Z_OK) return Z_DEFAULT_STRATEGY;
            z.next_in = const_cast<Bytef*>(at);
            z.avail_in = uInt(kSlice);
            z.next_out = buf.data();
            z.avail_out = uInt(buf.size());
            deflate(&z, Z_FINISH);
            (strategy == Z_RLE ? rle : lz) += z.total_out;
            deflateEnd(&z);
        }
    }
    return lz * 50 < rle * 49 ? Z_DEFAULT_STRATEGY : Z_RLE;  // LZ when it's over 2% smaller
}

// One deflate stream over data[0, n): zlib-wrapped (windowBits 15) or raw (-15). `dict` primes
// the window with the bytes before the block; `flush` is Z_FINISH, or Z_SYNC_FLUSH for a
// segment that another continues.
Bytes deflateBlock(const uint8_t* data, size_t n, int windowBits, const uint8_t* dict, size_t dictLen, int flush) {
    z_stream z{};
    if (deflateInit2(&z, kZlibLevel, Z_DEFLATED, windowBits, 8, strategyFor(data, n)) != Z_OK)
        throw std::runtime_error("zlib failed");
    if (dictLen) deflateSetDictionary(&z, dict, uInt(dictLen));
    Bytes out(deflateBound(&z, uLong(n)) + 16);  // + a sync flush marker
    z.next_in = const_cast<Bytef*>(data);
    z.avail_in = uInt(n);
    z.next_out = out.data();
    z.avail_out = uInt(out.size());
    const int r = deflate(&z, flush);
    out.resize(out.size() - z.avail_out);
    deflateEnd(&z);
    if (r != (flush == Z_FINISH ? Z_STREAM_END : Z_OK) || z.avail_in) throw std::runtime_error("zlib failed");
    return out;
}

// One zlib stream (the format of TIFF Deflate strips and OpenEXR ZIP blocks), for blocks that
// are compressed in parallel with each other.
Bytes zlibBlock(const uint8_t* data, size_t n) { return deflateBlock(data, n, 15, nullptr, 0, Z_FINISH); }

// A PNG chunk appended to `o`: length, type, data, CRC of type and data.
void appendChunk(Bytes& o, const char* type, const uint8_t* data, size_t n) {
    be32(o, uint32_t(n));
    o.insert(o.end(), type, type + 4);
    uLong crc = crc32_z(crc32(0, nullptr, 0), reinterpret_cast<const Bytef*>(type), 4);
    if (n) {  // crc32 of a null buffer is 0, not the running value
        o.insert(o.end(), data, data + n);
        crc = crc32_z(crc, data, n);
    }
    be32(o, uint32_t(crc));
}

// PNG's image data as IDAT chunks, one zlib stream compressed in parallel the way pigz does:
// 1 MB segments, each primed with the 32 KB before it as its dictionary and ended with a sync
// flush (the last one ends the stream). Each worker also wraps its segment in IDAT chunks of at
// most 1 MB (some readers, such as OpenCV, refuse larger ones) and computes their CRCs and its
// part of the Adler-32 checksum, which goes last in a chunk of its own.
std::vector<Bytes> idatParallel(const uint8_t* data, size_t n) {
    constexpr size_t kSeg = size_t(1) << 20, kDict = 32768, kMaxChunk = size_t(1) << 20;
    const int segs = int(std::max<size_t>(1, (n + kSeg - 1) / kSeg));
    std::vector<Bytes> chunks(segs + 1);
    std::vector<uLong> adlers(segs);
    forEach(segs, [&](int i) {
        const size_t at = size_t(i) * kSeg, len = std::min(kSeg, n - at);
        const size_t d = std::min(kDict, at);
        Bytes z = deflateBlock(data + at, len, -15, data + at - d, d, i + 1 == segs ? Z_FINISH : Z_SYNC_FLUSH);
        if (i == 0) z.insert(z.begin(), {0x78, 0x9C});  // deflate, 32 KB window, default level
        adlers[i] = adler32_z(1, data + at, len);
        Bytes& out = chunks[i];
        out.reserve(z.size() + 12 * (z.size() / kMaxChunk + 1));
        size_t pos = 0;
        do {
            const size_t c = std::min(kMaxChunk, z.size() - pos);
            appendChunk(out, "IDAT", z.data() + pos, c);
            pos += c;
        } while (pos < z.size());
    });
    uLong adler = adlers[0];
    for (int i = 1; i < segs; ++i)
        adler = adler32_combine(adler, adlers[i], z_off_t(std::min(kSeg, n - size_t(i) * kSeg)));
    Bytes tail;
    be32(tail, uint32_t(adler));
    Bytes& last = chunks[segs];
    appendChunk(last, "IDAT", tail.data(), tail.size());
    return chunks;
}

struct Span {
    const uint8_t* data;
    size_t size;
};

// Writes the pieces one after another, so large outputs aren't first copied into one buffer.
bool writeFile(const std::string& pathU8, const std::vector<Span>& pieces, std::string& err) {
    std::ofstream f(u8ToPath(pathU8), std::ios::binary);
    for (const Span& s : pieces)
        if (!f || !f.write(reinterpret_cast<const char*>(s.data), std::streamsize(s.size))) break;
    if (!f || !f.flush()) {
        err = "could not write file";
        return false;
    }
    return true;
}

bool writeFile(const std::string& pathU8, const Bytes& data, std::string& err) {
    return writeFile(pathU8, {{data.data(), data.size()}}, err);
}

bool opaque(const Image& img) {
    for (size_t i = 0, n = img.pixelCount(); i < n; ++i)
        if (img.px[i * 4 + 3] < 1.0f) return false;
    return true;
}

// Display values 0..1 to integers 0..maxV, `comp` channels per pixel, row-major. Rounds as
// std::lround of the float product did (half away from zero), without its library call.
template <class T>
std::vector<T> quantise(const Image& img, int comp, float maxV) {
    std::vector<T> out(img.pixelCount() * comp);
    parallelFor(img.h, [&](int y) {
        const float* s = img.pixel(size_t(y) * img.w);
        T* d = out.data() + size_t(y) * img.w * comp;
        for (int x = 0; x < img.w; ++x, s += 4, d += comp)
            for (int c = 0; c < comp; ++c) {
                const float v = s[c];
                const float p = std::clamp(v == v ? v : 0.0f, 0.0f, 1.0f) * maxV;
                d[c] = T(double(p) + 0.5);  // exact in double, so this is lround for p >= 0
            }
    });
    return out;
}

// ---------------------------------------------------------------- PNG

Bytes pngChunk(const char* type, const Bytes& data) {
    Bytes c;
    appendChunk(c, type, data.data(), data.size());
    return c;
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

// One row's PNG filter: per row, the filter with the smallest sum of absolute differences (the
// heuristic libpng and stb use). Each filter is its own loop, so the compiler can vectorise it.
void filterRow(const uint8_t* cur, const uint8_t* prev, size_t n, int bpp, uint8_t* out, uint8_t* trial) {
    auto score = [&](const uint8_t* t) {
        long s = 0;
        for (size_t i = 0; i < n; ++i) s += std::abs(int(int8_t(t[i])));
        return s;
    };
    const size_t b = std::min(size_t(bpp), n);
    long best = score(cur);
    out[0] = 0;
    std::memcpy(out + 1, cur, n);
    auto consider = [&](int f) {
        const long s = score(trial);
        if (s < best) {
            best = s;
            out[0] = uint8_t(f);
            std::memcpy(out + 1, trial, n);
        }
    };
    // Sub
    for (size_t i = 0; i < b; ++i) trial[i] = cur[i];
    for (size_t i = b; i < n; ++i) trial[i] = uint8_t(cur[i] - cur[i - bpp]);
    consider(1);
    // Up
    for (size_t i = 0; i < n; ++i) trial[i] = uint8_t(cur[i] - prev[i]);
    consider(2);
    // Average
    for (size_t i = 0; i < b; ++i) trial[i] = uint8_t(cur[i] - (prev[i] >> 1));
    for (size_t i = b; i < n; ++i) trial[i] = uint8_t(cur[i] - ((cur[i - bpp] + prev[i]) >> 1));
    consider(3);
    // Paeth
    for (size_t i = 0; i < b; ++i) trial[i] = uint8_t(cur[i] - prev[i]);  // a = c = 0: predicts b
    for (size_t i = b; i < n; ++i) {
        const int a = cur[i - bpp], bb = prev[i], c = prev[i - bpp];
        const int p = a + bb - c, pa = std::abs(p - a), pb = std::abs(p - bb), pc = std::abs(p - c);
        trial[i] = uint8_t(cur[i] - (pa <= pb && pa <= pc ? a : pb <= pc ? bb : c));
    }
    consider(4);
}

// 8- or 16-bit PNG: rows filtered in parallel, then compressed in parallel.
bool writePng(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err) {
    const int comp = opaque(img) ? 3 : 4;
    const bool sixteen = opt.depth >= 16;
    std::vector<uint16_t> px16;
    std::vector<uint8_t> px8;
    if (sixteen) px16 = quantise<uint16_t>(img, comp, 65535.0f);
    else px8 = quantise<uint8_t>(img, comp, 255.0f);
    const int bpp = comp * (sixteen ? 2 : 1);
    const size_t rowBytes = size_t(img.w) * bpp;
    Bytes filtered(size_t(img.h) * (rowBytes + 1));
    parallelForChunks(img.h, [&](int y0, int y1) {
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
        if (y0 > 0) load(prev, y0 - 1);
        for (int y = y0; y < y1; ++y) {
            load(cur, y);
            filterRow(cur.data(), prev.data(), rowBytes, bpp, filtered.data() + size_t(y) * (rowBytes + 1), trial.data());
            std::swap(cur, prev);
        }
    });
    px16 = {};
    px8 = {};
    Bytes head = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    Bytes ihdr;
    be32(ihdr, uint32_t(img.w));
    be32(ihdr, uint32_t(img.h));
    // Bit depth, colour type RGB(A), deflate, adaptive filtering, no interlace.
    ihdr.insert(ihdr.end(), {uint8_t(sixteen ? 16 : 8), uint8_t(comp == 4 ? 6 : 2), 0, 0, 0});
    appendChunk(head, "IHDR", ihdr.data(), ihdr.size());
    if (opt.srgb) {
        Bytes s = pngSrgbChunks();
        head.insert(head.end(), s.begin(), s.end());
    }
    const std::vector<Bytes> idat = idatParallel(filtered.data(), filtered.size());
    Bytes end;
    appendChunk(end, "IEND", nullptr, 0);
    std::vector<Span> pieces = {{head.data(), head.size()}};
    for (const Bytes& c : idat) pieces.push_back({c.data(), c.size()});
    pieces.push_back({end.data(), end.size()});
    return writeFile(pathU8, pieces, err);
}

void appendBytes(void* ctx, void* data, int size) {
    auto* out = static_cast<Bytes*>(ctx);
    out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

// ---------------------------------------------------------------- JPEG

// Where a JPEG's entropy-coded data starts (after the SOS segment), with the offsets of the SOF0
// height and the SOS marker; 0 if the headers don't parse.
size_t jpegScanStart(const Bytes& j, size_t& sofHeight, size_t& sos) {
    size_t pos = 2;
    while (pos + 4 <= j.size() && j[pos] == 0xFF) {
        const size_t len = size_t(j[pos + 2]) << 8 | j[pos + 3];
        if (j[pos + 1] == 0xC0) sofHeight = pos + 5;
        if (j[pos + 1] == 0xDA) {
            sos = pos;
            return pos + 2 + len;
        }
        pos += 2 + len;
    }
    return 0;
}

// Baseline JPEG through stb, encoded in strips in parallel. stb writes each strip as a complete
// JPEG. With a restart interval of one strip's MCUs, a decoder resets its DC predictions at each
// strip, just as each of stb's encodes started from zero, so the strips join into one file: the
// first strip's headers (with the full height and a DRI segment added), then each strip's
// entropy-coded data, with an RSTn marker between strips. It decodes to exactly the pixels of a
// single stb encode.
bool encodeJpeg(const uint8_t* rgb, int w, int h, int quality, Bytes& jpg) {
    const int mcu = quality <= 90 ? 16 : 8;  // stb subsamples chroma (4:2:0) at 90 and below
    const int mcusPerRow = (w + mcu - 1) / mcu, mcuRows = (h + mcu - 1) / mcu;
    // Whole MCU rows per strip: enough strips for every worker, within DRI's 16-bit interval.
    const int perStrip = std::min(std::max(1, mcuRows / (parallel::workerCount() * 4)), 65535 / mcusPerRow);
    const int strips = perStrip > 0 ? (mcuRows + perStrip - 1) / perStrip : 1;
    if (strips < 2) return stbi_write_jpg_to_func(appendBytes, &jpg, w, h, 3, rgb, quality) && jpg.size() >= 4;
    std::vector<Bytes> parts(strips);
    std::vector<size_t> scan(strips);
    std::atomic<bool> ok = true;
    size_t sofHeight = 0, sos = 0;
    forEach(strips, [&](int s) {
        const int y0 = s * perStrip * mcu, y1 = std::min(h, y0 + perStrip * mcu);
        size_t sofAt = 0, sosAt = 0;
        if (!stbi_write_jpg_to_func(appendBytes, &parts[s], w, y1 - y0, 3, rgb + size_t(y0) * w * 3, quality) ||
            !(scan[s] = jpegScanStart(parts[s], sofAt, sosAt)) || parts[s].size() < scan[s] + 2) {
            ok = false;
            return;
        }
        if (s == 0) sofHeight = sofAt, sos = sosAt;
    });
    if (!ok || !sofHeight) return false;
    const Bytes& first = parts[0];
    jpg.assign(first.begin(), first.begin() + sos);
    jpg[sofHeight] = uint8_t(h >> 8);
    jpg[sofHeight + 1] = uint8_t(h);
    const int interval = perStrip * mcusPerRow;
    jpg.insert(jpg.end(), {0xFF, 0xDD, 0, 4, uint8_t(interval >> 8), uint8_t(interval)});  // DRI
    jpg.insert(jpg.end(), first.begin() + sos, first.begin() + scan[0]);                    // SOS
    size_t total = jpg.size() + 2;
    for (int s = 0; s < strips; ++s) total += parts[s].size() - scan[s];
    jpg.reserve(total);
    for (int s = 0; s < strips; ++s) {
        // Each strip's data ends with its EOI marker: replaced by RSTn, and by EOI after the last.
        jpg.insert(jpg.end(), parts[s].begin() + scan[s], parts[s].end() - 2);
        if (s + 1 < strips) jpg.insert(jpg.end(), {0xFF, uint8_t(0xD0 + s % 8)});
        Bytes().swap(parts[s]);
    }
    jpg.insert(jpg.end(), {0xFF, 0xD9});
    return true;
}

bool writeJpeg(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err) {
    const std::vector<uint8_t> bytes = quantise<uint8_t>(img, 3, 255.0f);
    Bytes jpg;
    if (!encodeJpeg(bytes.data(), img.w, img.h, std::clamp(opt.jpegQuality, 1, 100), jpg)) {
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
    forEach(strips, [&](int s) {
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

    px16 = {};
    px8 = {};
    Bytes head = tiff::header();
    std::vector<uint32_t> offsets, counts;
    size_t pos = head.size();
    for (const Bytes& p : packed) {
        offsets.push_back(uint32_t(pos));
        counts.push_back(uint32_t(p.size()));
        pos += p.size();
    }
    if (pos > 0xFFFFFFF0u) {
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
    Bytes dir;
    tiff::set32(head, 4, ifd.write(dir, pos));
    std::vector<Span> pieces = {{head.data(), head.size()}};
    for (const Bytes& p : packed) pieces.push_back({p.data(), p.size()});
    pieces.push_back({dir.data(), dir.size()});
    return writeFile(pathU8, pieces, err);
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
    forEach(blocks, [&](int b) {
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
    std::vector<Span> pieces = {{out.data(), out.size()}};
    for (const Bytes& p : packed) pieces.push_back({p.data(), p.size()});
    return writeFile(pathU8, pieces, err);
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
