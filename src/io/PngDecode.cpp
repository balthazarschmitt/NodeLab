#include "io/PngDecode.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <zlib.h>


namespace png {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Paeth for pixels of bpp bytes. The byte loop (one chain through the left neighbour, with
// branches that random-looking residuals mispredict) took about 0.4 s at 24 MP. Here the left pixel
// stays in registers, each channel is its own chain, and the choice is branchless.
template <size_t bpp>
void paethRow(uint8_t* row, const uint8_t* prior, size_t n) {
    int a[bpp], c[bpp];
    for (size_t k = 0; k < bpp; ++k) {
        row[k] = uint8_t(row[k] + prior[k]);  // Paeth(0, b, 0) = b
        a[k] = row[k];
        c[k] = prior[k];
    }
    for (size_t i = bpp; i < n; i += bpp) {
        for (size_t k = 0; k < bpp; ++k) {
            const int b = prior[i + k];
            const int pa = std::abs(b - c[k]), pb = std::abs(a[k] - c[k]), pc = std::abs(a[k] + b - 2 * c[k]);
            const int bc = pb <= pc ? b : c[k];  // ties favour a, then b
            const int pred = pa <= std::min(pb, pc) ? a[k] : bc;
            a[k] = uint8_t(row[i + k] + pred);
            row[i + k] = uint8_t(a[k]);
            c[k] = b;
        }
    }
}

// Undoes one row's filter in place. prior is the previous unfiltered row (zeros for the first,
// which is what stb's first-row filter variants amount to).
bool unfilterRow(int filter, uint8_t* row, const uint8_t* prior, size_t n, size_t bpp) {
    switch (filter) {
        case 0: return true;
        case 2:
            for (size_t i = 0; i < n; ++i) row[i] = uint8_t(row[i] + prior[i]);
            return true;
        case 1:
            for (size_t i = bpp; i < n; ++i) row[i] = uint8_t(row[i] + row[i - bpp]);
            return true;
        case 3:
            for (size_t i = 0; i < bpp; ++i) row[i] = uint8_t(row[i] + (prior[i] >> 1));
            for (size_t i = bpp; i < n; ++i) row[i] = uint8_t(row[i] + ((row[i - bpp] + prior[i]) >> 1));
            return true;
        case 4:
            switch (bpp) {
                case 1: paethRow<1>(row, prior, n); return true;
                case 2: paethRow<2>(row, prior, n); return true;
                case 3: paethRow<3>(row, prior, n); return true;
                case 4: paethRow<4>(row, prior, n); return true;
                case 6: paethRow<6>(row, prior, n); return true;
                default: paethRow<8>(row, prior, n); return true;
            }
        default: return false;  // stb rejects other filter types
    }
}

}  // namespace

bool decode(const uint8_t* f, size_t size, Decoded& out) {
    if (size < 8 + 25 || std::memcmp(f, "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    // IHDR must come first.
    if (be32(f + 8) != 13 || std::memcmp(f + 12, "IHDR", 4) != 0) return false;
    const uint8_t* ih = f + 16;
    const uint32_t w = be32(ih), h = be32(ih + 4);
    const int depth = ih[8], colorType = ih[9], interlace = ih[12];
    if (ih[10] != 0 || ih[11] != 0 || interlace != 0) return false;
    if (depth != 8 && depth != 16) return false;
    int channels = 0;
    switch (colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return false;  // palette
    }
    // stb's own limit (STBI_MAX_DIMENSIONS), and keep the sizes well inside size_t.
    if (w == 0 || h == 0 || w > (1u << 24) || h > (1u << 24) || uint64_t(w) * h > (uint64_t(1) << 31)) return false;

    // Gather the IDAT chunks; anything stb treats specially sends the file to stb.
    struct Span {
        const uint8_t* p;
        size_t n;
    };
    std::vector<Span> idat;
    bool ended = false;
    for (size_t i = 8 + 25; i + 12 <= size;) {
        const size_t len = be32(f + i);
        if (len > size - i - 12) return false;
        const uint8_t* type = f + i + 4;
        if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.push_back({f + i + 8, len});
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            ended = true;
            break;
        } else if (std::memcmp(type, "tRNS", 4) == 0 || std::memcmp(type, "CgBI", 4) == 0 ||
                   std::memcmp(type, "PLTE", 4) == 0 || std::memcmp(type, "IHDR", 4) == 0) {
            return false;
        } else if (!(type[0] & 32)) {
            return false;  // an unknown critical chunk: stb refuses the file
        }
        i += 12 + len;
    }
    if (!ended || idat.empty()) return false;

    const size_t sampleBytes = size_t(depth / 8);
    const size_t bpp = size_t(channels) * sampleBytes;
    const size_t rowBytes = size_t(w) * bpp;
    const size_t stride = rowBytes + 1;  // filter byte + row
    std::vector<uint8_t, UninitAllocator<uint8_t>> raw(stride * h);  // every byte is inflated into

    // Inflating is serial and so is unfiltering (each row predicts from the one above), so they
    // run as a pipeline: a second thread unfilters each row as soon as the inflate has written it.
    // At 24 MP this roughly halves the time, since the two take about as long as each other.
    constexpr size_t kFailed = ~size_t(0);
    std::atomic<size_t> produced{0};
    std::atomic<bool> badFilter{false};
    std::thread unfilter([&] {
        std::vector<uint8_t> zeros(rowBytes, 0);
        size_t have = 0;
        for (uint32_t y = 0; y < h; ++y) {
            const size_t need = (size_t(y) + 1) * stride;
            for (;;) {
                if (have == kFailed) return;
                if (have >= need) break;
                produced.wait(have, std::memory_order_acquire);
                have = produced.load(std::memory_order_acquire);
            }
            uint8_t* row = raw.data() + size_t(y) * stride;
            const uint8_t* prior = y ? row - stride + 1 : zeros.data();
            if (!unfilterRow(row[0], row + 1, prior, rowBytes, bpp)) {
                badFilter.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });

    z_stream zs{};
    bool ok = inflateInit(&zs) == Z_OK;
    size_t done = 0;
    int zr = Z_OK;
    for (const Span& s : idat) {
        if (!ok) break;
        zs.next_in = const_cast<Bytef*>(s.p);
        zs.avail_in = uInt(s.n);
        while (zs.avail_in > 0 && zr == Z_OK && done < raw.size() && !badFilter.load(std::memory_order_relaxed)) {
            // Small steps so the unfilter thread can follow closely.
            zs.next_out = raw.data() + done;
            zs.avail_out = uInt(std::min<size_t>(raw.size() - done, 256 << 10));
            zr = inflate(&zs, Z_NO_FLUSH);
            done = size_t(zs.next_out - raw.data());
            produced.store(done, std::memory_order_release);
            produced.notify_one();
        }
        if (zr != Z_OK || done == raw.size()) break;
    }
    if (ok) inflateEnd(&zs);
    // A damaged stream, or too few bytes: let stb decide what to do with the file. (More data
    // than the image needs is ignored, as stb does.)
    ok = ok && (zr == Z_OK || zr == Z_STREAM_END) && done == raw.size();
    if (!ok) {
        produced.store(kFailed, std::memory_order_release);
        produced.notify_one();
    }
    unfilter.join();
    if (!ok || badFilter.load()) return false;

    out.w = int(w);
    out.h = int(h);
    out.channels = channels;
    out.sixteen = depth == 16;
    out.stride = stride;
    out.raw = std::move(raw);
    return true;
}

void Decoded::expandRow(int y, uint8_t* d) const {
    const uint8_t* src = raw.data() + size_t(y) * stride + 1;
    switch (channels) {
        case 4: std::memcpy(d, src, size_t(w) * 4); break;
        case 3:
            for (int x = 0; x < w; ++x, d += 4, src += 3) d[0] = src[0], d[1] = src[1], d[2] = src[2], d[3] = 255;
            break;
        case 2:
            for (int x = 0; x < w; ++x, d += 4, src += 2) d[0] = d[1] = d[2] = src[0], d[3] = src[1];
            break;
        default:
            for (int x = 0; x < w; ++x, d += 4, ++src) d[0] = d[1] = d[2] = src[0], d[3] = 255;
            break;
    }
}

void Decoded::expandRow(int y, uint16_t* d) const {
    const uint8_t* src = raw.data() + size_t(y) * stride + 1;
    auto at = [&](size_t i) { return uint16_t(src[2 * i] << 8 | src[2 * i + 1]); };
    for (int x = 0; x < w; ++x, d += 4) {
        const size_t s = size_t(x) * size_t(channels);
        switch (channels) {
            case 1: d[0] = d[1] = d[2] = at(s), d[3] = 0xFFFF; break;
            case 2: d[0] = d[1] = d[2] = at(s), d[3] = at(s + 1); break;
            case 3: d[0] = at(s), d[1] = at(s + 1), d[2] = at(s + 2), d[3] = 0xFFFF; break;
            default: d[0] = at(s), d[1] = at(s + 1), d[2] = at(s + 2), d[3] = at(s + 3); break;
        }
    }
}

}  // namespace png
