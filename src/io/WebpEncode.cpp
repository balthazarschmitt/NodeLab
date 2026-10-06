#include "io/WebpEncode.h"

#include <algorithm>
#include <cstdlib>
#include <queue>
#include <utility>

#include "core/Parallel.h"

namespace webp {

namespace {

using Bytes = std::vector<uint8_t>;

// VP8L reads bits from the least significant end of each byte.
struct BitWriter {
    Bytes out;
    uint64_t acc = 0;
    int n = 0;
    void put(uint32_t v, int bits) {
        acc |= uint64_t(v) << n;
        n += bits;
        while (n >= 8) {
            out.push_back(uint8_t(acc));
            acc >>= 8;
            n -= 8;
        }
    }
    void flush() {
        if (n > 0) out.push_back(uint8_t(acc));
        acc = 0, n = 0;
    }
};

// Huffman code lengths no longer than `limit`: when the tree is too deep, the counts are halved
// (keeping every used symbol) and it is built again, which flattens it.
std::vector<uint8_t> codeLengths(const std::vector<uint32_t>& freq, int limit) {
    std::vector<uint8_t> len(freq.size(), 0);
    std::vector<uint64_t> f(freq.begin(), freq.end());
    struct TreeNode {
        int left, right;  // left < 0: a leaf for symbol `right`
    };
    for (;;) {
        std::vector<TreeNode> nodes;
        using Entry = std::pair<uint64_t, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> q;
        for (size_t i = 0; i < f.size(); ++i)
            if (f[i]) {
                q.push({f[i], int(nodes.size())});
                nodes.push_back({-1, int(i)});
            }
        if (nodes.empty()) return len;
        if (nodes.size() == 1) {
            len[size_t(nodes[0].right)] = 1;
            return len;
        }
        while (q.size() > 1) {
            const Entry a = q.top();
            q.pop();
            const Entry b = q.top();
            q.pop();
            q.push({a.first + b.first, int(nodes.size())});
            nodes.push_back({a.second, b.second});
        }
        std::vector<int> depth(nodes.size(), 0);
        int deepest = 0;
        for (int i = int(nodes.size()) - 1; i >= 0; --i) {
            if (nodes[size_t(i)].left < 0) {
                deepest = std::max(deepest, depth[size_t(i)]);
                continue;
            }
            depth[size_t(nodes[size_t(i)].left)] = depth[size_t(nodes[size_t(i)].right)] = depth[size_t(i)] + 1;
        }
        if (deepest <= limit) {
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].left < 0) len[size_t(nodes[i].right)] = uint8_t(depth[i]);
            return len;
        }
        for (uint64_t& v : f)
            if (v) v = std::max<uint64_t>(1, v / 2);
    }
}

// A prefix code ready to write: canonical codes, bit-reversed because VP8L reads a code's first
// bit from the stream's least significant end.
struct Code {
    std::vector<uint8_t> len;
    std::vector<uint16_t> bits;
    bool single = false;  // one symbol: the decoder reads it with no bits
    void write(BitWriter& bw, int sym) const {
        if (!single) bw.put(bits[size_t(sym)], len[size_t(sym)]);
    }
};

Code canonical(const std::vector<uint8_t>& len) {
    Code c;
    c.len = len;
    c.bits.assign(len.size(), 0);
    int count[16] = {}, next[16] = {};
    for (uint8_t l : len) ++count[l];
    count[0] = 0;
    for (int l = 1, code = 0; l < 16; ++l) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    for (size_t s = 0; s < len.size(); ++s) {
        const int l = len[s];
        if (!l) continue;
        const int code = next[l]++;
        int r = 0;
        for (int k = 0; k < l; ++k) r |= ((code >> k) & 1) << (l - 1 - k);
        c.bits[s] = uint16_t(r);
    }
    return c;
}

constexpr int kCodeLengthOrder[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// Writes the prefix code for these symbol counts and returns it.
Code writeCode(BitWriter& bw, std::vector<uint32_t> freq) {
    int used = 0, only = 0;
    for (size_t s = 0; s < freq.size(); ++s)
        if (freq[s]) ++used, only = int(s);
    if (used <= 1 && only < 256) {
        // A simple code with one symbol (an unused alphabet gets symbol 0).
        bw.put(1, 1);
        bw.put(0, 1);
        if (only < 2) bw.put(0, 1), bw.put(uint32_t(only), 1);
        else bw.put(1, 1), bw.put(uint32_t(only), 8);
        Code c;
        c.single = true;
        return c;
    }
    if (used == 1) freq[only == 0 ? 1 : 0] = 1;  // a lone symbol past 255 gets a 1-bit partner

    const std::vector<uint8_t> len = codeLengths(freq, 15);
    // The lengths, with runs of zeros as code-length symbols 17 (3-10) and 18 (11-138).
    std::vector<std::pair<int, int>> tokens;
    for (size_t i = 0; i < len.size();) {
        if (len[i] != 0) {
            tokens.push_back({len[i], 0});
            ++i;
            continue;
        }
        size_t run = 0;
        while (i + run < len.size() && len[i + run] == 0) ++run;
        i += run;
        while (run >= 3) {
            const size_t r = run >= 11 ? std::min<size_t>(run, 138) : std::min<size_t>(run, 10);
            tokens.push_back(run >= 11 ? std::pair{18, int(r - 11)} : std::pair{17, int(r - 3)});
            run -= r;
        }
        for (; run; --run) tokens.push_back({0, 0});
    }
    std::vector<uint32_t> clFreq(19, 0);
    for (const auto& t : tokens) ++clFreq[size_t(t.first)];
    const std::vector<uint8_t> clLen = codeLengths(clFreq, 7);
    Code cl = canonical(clLen);
    cl.single = std::count_if(clLen.begin(), clLen.end(), [](uint8_t l) { return l != 0; }) == 1;
    int num = 19;
    while (num > 4 && clLen[size_t(kCodeLengthOrder[num - 1])] == 0) --num;
    bw.put(0, 1);  // normal code
    bw.put(uint32_t(num - 4), 4);
    for (int k = 0; k < num; ++k) bw.put(clLen[size_t(kCodeLengthOrder[k])], 3);
    bw.put(0, 1);  // lengths for the whole alphabet
    for (const auto& t : tokens) {
        cl.write(bw, t.first);
        if (t.first == 17) bw.put(uint32_t(t.second), 3);
        if (t.first == 18) bw.put(uint32_t(t.second), 7);
    }
    return canonical(len);
}

// VP8L's prefix coding of lengths and distance codes (1-based): a prefix symbol and extra bits.
void prefixEncode(int v, int& sym, int& extraBits, int& extra) {
    const int d = v - 1;
    if (d < 4) {
        sym = d, extraBits = 0, extra = 0;
        return;
    }
    int hb = 31;
    while (!(uint32_t(d) >> hb)) --hb;
    const int second = (d >> (hb - 1)) & 1;
    extraBits = hb - 1;
    sym = 2 * hb + second;
    extra = d & ((1 << extraBits) - 1);
}

constexpr int kMaxLength = 4096;
constexpr int kCacheBits = 10;
constexpr int kWindow = (1 << 20) - 120;  // the farthest distance the 40 distance codes reach
constexpr int kHashBits = 18;
constexpr int kBandRows = 128;
constexpr int kChain = 8;  // candidates tried per position

int cacheIndex(uint32_t argb) { return int((0x1e35a7bdu * argb) >> (32 - kCacheBits)); }

// An entropy-coded image: one group of five prefix codes, a colour cache, and backward references
// (LZ77) found through a hash chain over three-pixel strings, plus the pixel on the left (plane
// code 2: runs) and above (plane code 1: repeated rows).
void writeImage(BitWriter& bw, const std::vector<uint32_t>& px, int w, bool main) {
    bw.put(1, 1);
    bw.put(kCacheBits, 4);
    if (main) bw.put(0, 1);  // no meta prefix codes
    struct Token {
        uint32_t argb;
        int length;  // 0: a literal, -1: a colour cache hit (index in argb)
        int dcode;   // the distance code: 1 above, 2 left, else distance + 120
    };
    const size_t n = px.size();
    auto matchLen = [&](size_t i, size_t dist, size_t stop) {
        int l = 0;
        const int most = int(std::min<size_t>(kMaxLength, stop - i));
        while (l < most && px[i + size_t(l)] == px[i + size_t(l) - dist]) ++l;
        return l;
    };
    // LZ77 in bands of rows, in parallel: a match may reach back into earlier bands (the decoder
    // has those pixels) but the hash chain only holds the band's own positions. Bands are a fixed
    // number of rows, so the file doesn't depend on the thread count.
    const size_t bandPixels = size_t(w) * kBandRows;
    const int bands = int((n + bandPixels - 1) / bandPixels);
    std::vector<std::vector<Token>> bandTokens(static_cast<size_t>(bands));
    parallelFor(bands, [&](int b) {
        const size_t begin = size_t(b) * bandPixels, stop = std::min(n, begin + bandPixels);
        std::vector<Token>& tokens = bandTokens[size_t(b)];
        std::vector<int> head(size_t(1) << kHashBits, -1), prev(stop - begin, -1);
        auto hashAt = [&](size_t i) {
            uint32_t h = px[i] * 0x9E3779B1u;
            h = (h ^ (h >> 15) ^ px[i + 1]) * 0x85EBCA77u;
            h = (h ^ (h >> 13) ^ px[i + 2]) * 0xC2B2AE3Du;
            return size_t(h >> (32 - kHashBits));
        };
        auto insert = [&](size_t i) {
            if (i + 2 >= stop) return;
            const size_t h = hashAt(i);
            prev[i - begin] = head[h];
            head[h] = int(i - begin);
        };
        for (size_t i = begin; i < stop;) {
            int best = 0, dcode = 0;
            // Above and left first: their codes are the cheapest, so they win ties.
            const std::pair<size_t, int> near[2] = {{1, 2}, {size_t(w), 1}};
            for (const auto& [dist, code] : near) {
                if (i < dist) continue;
                const int l = matchLen(i, dist, stop);
                if (l > best) best = l, dcode = code;
            }
            if (i + 2 < stop && best < kMaxLength) {
                int cand = head[hashAt(i)];
                for (int tries = 0; cand >= 0 && tries < kChain; ++tries, cand = prev[size_t(cand)]) {
                    const size_t dist = i - begin - size_t(cand);
                    if (dist > size_t(kWindow)) break;
                    if (dist == 1 || dist == size_t(w)) continue;
                    // Can't beat the best without matching one pixel past it: a cheap rejection.
                    const size_t past = i + size_t(best) + 1;
                    if (past < stop && px[past] != px[past - dist]) continue;
                    const int l = matchLen(i, dist, stop);
                    // A far distance costs more bits: it has to be clearly longer.
                    if (l > best + 1) best = l, dcode = int(dist) + 120;
                }
            }
            if (best >= 3) {
                tokens.push_back({0, best, dcode});
                for (int k = 0; k < best; ++k) insert(i + size_t(k));
                i += size_t(best);
            } else {
                tokens.push_back({px[i], 0, 0});
                insert(i);
                ++i;
            }
        }
    });
    // The colour cache follows every pixel in order, so literals become cache hits in one
    // sequential pass.
    std::vector<Token> tokens;
    size_t total = 0;
    for (const auto& t : bandTokens) total += t.size();
    tokens.reserve(total);
    std::vector<uint32_t> cache(size_t(1) << kCacheBits, 0);
    std::vector<bool> cached(size_t(1) << kCacheBits, false);
    size_t pos = 0;
    for (auto& band : bandTokens) {
        for (Token t : band) {
            if (t.length > 0) {
                for (int k = 0; k < t.length; ++k) {
                    const int c = cacheIndex(px[pos + size_t(k)]);
                    cache[size_t(c)] = px[pos + size_t(k)], cached[size_t(c)] = true;
                }
                pos += size_t(t.length);
            } else {
                const int c = cacheIndex(t.argb);
                if (cached[size_t(c)] && cache[size_t(c)] == t.argb) t = {uint32_t(c), -1, 0};
                cache[size_t(c)] = px[pos], cached[size_t(c)] = true;
                ++pos;
            }
            tokens.push_back(t);
        }
        std::vector<Token>().swap(band);
    }
    std::vector<uint32_t> green(256 + 24 + (size_t(1) << kCacheBits), 0), red(256, 0), blue(256, 0), alpha(256, 0),
        dist(40, 0);
    for (const Token& t : tokens) {
        if (t.length > 0) {
            int sym, eb, ex;
            prefixEncode(t.length, sym, eb, ex);
            ++green[size_t(256 + sym)];
            prefixEncode(t.dcode, sym, eb, ex);
            ++dist[size_t(sym)];
        } else if (t.length < 0) {
            ++green[280 + t.argb];
        } else {
            ++green[(t.argb >> 8) & 0xff], ++red[(t.argb >> 16) & 0xff], ++blue[t.argb & 0xff];
            ++alpha[t.argb >> 24];
        }
    }
    const Code cg = writeCode(bw, green), cr = writeCode(bw, red), cb = writeCode(bw, blue), ca = writeCode(bw, alpha),
               cd = writeCode(bw, dist);
    for (const Token& t : tokens) {
        if (t.length > 0) {
            int sym, eb, ex;
            prefixEncode(t.length, sym, eb, ex);
            cg.write(bw, 256 + sym);
            if (eb) bw.put(uint32_t(ex), eb);
            prefixEncode(t.dcode, sym, eb, ex);
            cd.write(bw, sym);
            if (eb) bw.put(uint32_t(ex), eb);
        } else if (t.length < 0) {
            cg.write(bw, 280 + int(t.argb));
        } else {
            cg.write(bw, int((t.argb >> 8) & 0xff));
            cr.write(bw, int((t.argb >> 16) & 0xff));
            cb.write(bw, int(t.argb & 0xff));
            ca.write(bw, int(t.argb >> 24));
        }
    }
}

uint32_t avg2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b); }

int chan(uint32_t v, int s) { return int((v >> s) & 0xff); }

uint32_t select(uint32_t l, uint32_t t, uint32_t tl) {
    int toL = 0, toT = 0;  // the gradient estimate's distance to each
    for (int s = 0; s < 32; s += 8) {
        toL += std::abs(chan(t, s) - chan(tl, s));
        toT += std::abs(chan(l, s) - chan(tl, s));
    }
    return toL < toT ? l : t;
}

uint32_t clampAddSub(uint32_t l, uint32_t t, uint32_t tl) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) r |= uint32_t(std::clamp(chan(l, s) + chan(t, s) - chan(tl, s), 0, 255)) << s;
    return r;
}

uint32_t predict(int mode, uint32_t l, uint32_t t, uint32_t tl) {
    switch (mode) {
        case 1: return l;
        case 2: return t;
        case 7: return avg2(l, t);
        case 11: return select(l, t, tl);
        case 12: return clampAddSub(l, t, tl);
        default: return 0xff000000u;
    }
}

uint32_t subPixels(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) r |= uint32_t((chan(a, s) - chan(b, s)) & 0xff) << s;
    return r;
}

// The VP8L prediction for pixel (x, y): fixed at the edges, the tile's mode inside.
uint32_t predictAt(const std::vector<uint32_t>& p, int w, int x, int y, int mode) {
    const size_t i = size_t(y) * size_t(w) + size_t(x);
    if (y == 0) return x == 0 ? 0xff000000u : p[i - 1];
    if (x == 0) return p[i - size_t(w)];
    return predict(mode, p[i - 1], p[i - size_t(w)], p[i - size_t(w) - 1]);
}

constexpr int kTileBits = 5;
constexpr int kModes[] = {1, 2, 7, 11, 12};

}  // namespace

std::vector<uint8_t> encodeLossless(const uint8_t* rgba, int w, int h, bool alphaUsed) {
    BitWriter bw;
    bw.put(0x2f, 8);
    bw.put(uint32_t(w - 1), 14);
    bw.put(uint32_t(h - 1), 14);
    bw.put(alphaUsed ? 1 : 0, 1);
    bw.put(0, 3);

    // Subtract green: red and blue keep only their difference from green.
    std::vector<uint32_t> p(size_t(w) * size_t(h));
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const uint8_t* s = rgba + i * 4;
            const uint32_t r = uint8_t(s[0] - s[1]), b = uint8_t(s[2] - s[1]);
            p[i] = uint32_t(s[3]) << 24 | r << 16 | uint32_t(s[1]) << 8 | b;
        }
    });
    bw.put(1, 1);
    bw.put(2, 2);  // SUBTRACT_GREEN_TRANSFORM

    // Predictor: each 32x32 tile takes the mode with the smallest residuals.
    const int tw = (w + (1 << kTileBits) - 1) >> kTileBits, th = (h + (1 << kTileBits) - 1) >> kTileBits;
    std::vector<uint32_t> modes(size_t(tw) * size_t(th));
    std::vector<uint32_t> res(p.size());
    parallelFor(th, [&](int ty) {
        const int y0 = ty << kTileBits, y1 = std::min(h, y0 + (1 << kTileBits));
        for (int tx = 0; tx < tw; ++tx) {
            const int x0 = tx << kTileBits, x1 = std::min(w, x0 + (1 << kTileBits));
            int bestMode = 1;
            long bestCost = -1;
            for (int mode : kModes) {
                long cost = 0;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const uint32_t r = subPixels(p[size_t(y) * size_t(w) + size_t(x)], predictAt(p, w, x, y, mode));
                        for (int s = 0; s < 32; s += 8) cost += std::abs(int(int8_t(uint8_t(r >> s))));
                    }
                if (bestCost < 0 || cost < bestCost) bestCost = cost, bestMode = mode;
            }
            modes[size_t(ty) * size_t(tw) + size_t(tx)] = 0xff000000u | uint32_t(bestMode) << 8;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const size_t i = size_t(y) * size_t(w) + size_t(x);
                    res[i] = subPixels(p[i], predictAt(p, w, x, y, bestMode));
                }
        }
    });
    bw.put(1, 1);
    bw.put(0, 2);  // PREDICTOR_TRANSFORM
    bw.put(kTileBits - 2, 3);
    writeImage(bw, modes, tw, false);
    bw.put(0, 1);  // no more transforms

    writeImage(bw, res, w, true);
    bw.flush();
    return std::move(bw.out);
}

namespace {

void le32(Bytes& o, uint32_t v) {
    for (int s = 0; s < 32; s += 8) o.push_back(uint8_t(v >> s));
}
void le24(Bytes& o, uint32_t v) {
    for (int s = 0; s < 24; s += 8) o.push_back(uint8_t(v >> s));
}
void chunk(Bytes& o, const char* type, const Bytes& data) {
    o.insert(o.end(), type, type + 4);
    le32(o, uint32_t(data.size()));
    o.insert(o.end(), data.begin(), data.end());
    if (data.size() & 1) o.push_back(0);  // chunks are padded to even sizes
}

}  // namespace

std::vector<uint8_t> container(const std::vector<uint8_t>& vp8l, int w, int h, bool alphaUsed,
                               const std::vector<uint8_t>& icc, const std::vector<uint8_t>& exif,
                               const std::vector<uint8_t>& xmp) {
    return container({{"VP8L", vp8l}}, w, h, alphaUsed, icc, exif, xmp);
}

std::vector<uint8_t> container(const std::vector<Chunk>& image, int w, int h, bool alphaUsed,
                               const std::vector<uint8_t>& icc, const std::vector<uint8_t>& exif,
                               const std::vector<uint8_t>& xmp) {
    Bytes body = {'W', 'E', 'B', 'P'};
    // A lossy picture's alpha is a separate ALPH chunk, which only the extended format allows.
    bool alphChunk = false;
    for (const Chunk& c : image) alphChunk |= c.type == "ALPH";
    if (!icc.empty() || !exif.empty() || !xmp.empty() || alphChunk) {
        Bytes x;
        x.push_back(uint8_t((icc.empty() ? 0 : 0x20) | (alphaUsed ? 0x10 : 0) | (exif.empty() ? 0 : 0x08) |
                            (xmp.empty() ? 0 : 0x04)));
        x.insert(x.end(), 3, 0);
        le24(x, uint32_t(w - 1));
        le24(x, uint32_t(h - 1));
        chunk(body, "VP8X", x);
        if (!icc.empty()) chunk(body, "ICCP", icc);
    }
    for (const Chunk& c : image) chunk(body, c.type.c_str(), c.data);
    if (!exif.empty()) chunk(body, "EXIF", exif);
    if (!xmp.empty()) chunk(body, "XMP ", xmp);
    Bytes file = {'R', 'I', 'F', 'F'};
    le32(file, uint32_t(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

}  // namespace webp
