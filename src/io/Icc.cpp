#include "io/Icc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <zlib.h>

#include "io/ImageCodecs.h"
#include "io/Paths.h"
#include "io/TiffDecode.h"

namespace icc {

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
double s15(const uint8_t* p) { return int32_t(be32(p)) / 65536.0; }

// The parts of a JPEG or PNG that can hold a profile, as a smaller file of the same format: the
// signature, then only the APP2 segments (JPEG) or iCCP chunk (PNG) up to the image data. Reading
// the whole file took longer than decoding a big 16-bit PNG's profile needs (0.4 s at 24 MP).
Bytes readProfileSegments(const std::string& pathU8) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) return {};
    uint8_t sig[8] = {};
    if (!f.read(reinterpret_cast<char*>(sig), 2)) return {};
    Bytes out(sig, sig + 2);
    auto take = [&](size_t n) {  // appends the next n bytes of the file
        const size_t at = out.size();
        out.resize(at + n);
        if (!f.read(reinterpret_cast<char*>(out.data() + at), std::streamsize(n))) {
            out.resize(at + size_t(f.gcount()));
            return false;
        }
        return true;
    };
    if (sig[0] == 0xFF && sig[1] == 0xD8) {
        // Same walk as jpegProfile; segments other than APP2 are skipped without reading them.
        uint8_t m[4];
        while (f.read(reinterpret_cast<char*>(m), 2) && m[0] == 0xFF) {
            const uint8_t marker = m[1];
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
            if (marker == 0xFF) {
                f.seekg(-1, std::ios::cur);
                continue;
            }
            if (marker == 0xDA || marker == 0xD9) break;
            if (!f.read(reinterpret_cast<char*>(m + 2), 2)) break;
            const size_t len = be16(m + 2);
            if (len < 2) break;
            if (marker == 0xE2) {
                out.insert(out.end(), m, m + 4);
                if (!take(len - 2)) break;
            } else {
                f.seekg(std::streamoff(len - 2), std::ios::cur);
            }
        }
        return out;
    }
    if (!f.read(reinterpret_cast<char*>(sig + 2), 6) || std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) return {};
    out.assign(sig, sig + 8);
    uint8_t h[8];
    while (f.read(reinterpret_cast<char*>(h), 8)) {
        const size_t len = be32(h);
        if (std::memcmp(h + 4, "IDAT", 4) == 0) break;
        if (std::memcmp(h + 4, "iCCP", 4) == 0) {
            out.insert(out.end(), h, h + 8);
            take(len + 4);
            break;
        }
        f.seekg(std::streamoff(len) + 4, std::ios::cur);
    }
    return out;
}

// JPEG: the profile is split over APP2 segments ("ICC_PROFILE\0", sequence number, count), which
// come before the image data.
Bytes jpegProfile(const Bytes& f) {
    std::vector<Bytes> parts;
    size_t i = 2;
    while (i + 4 <= f.size() && f[i] == 0xFF) {
        const uint8_t marker = f[i + 1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            i += 2;  // no length
            continue;
        }
        if (marker == 0xFF) {  // fill byte
            ++i;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) break;  // start of scan / end: no more headers
        const size_t len = be16(&f[i + 2]);
        if (len < 2 || i + 2 + len > f.size()) break;
        const uint8_t* seg = &f[i + 4];
        const size_t n = len - 2;
        if (marker == 0xE2 && n > 14 && std::memcmp(seg, "ICC_PROFILE\0", 12) == 0) {
            const int seq = seg[12], count = seg[13];
            if (count > 0 && seq >= 1 && seq <= count) {
                if (parts.size() < size_t(count)) parts.resize(size_t(count));
                parts[size_t(seq - 1)].assign(seg + 14, seg + n);
            }
        }
        i += 2 + len;
    }
    Bytes out;
    for (const Bytes& p : parts) {
        if (p.empty()) return {};  // a missing chunk: better no profile than a broken one
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// PNG: an iCCP chunk (name, compression method, zlib data) before the first IDAT.
Bytes pngProfile(const Bytes& f) {
    size_t i = 8;
    while (i + 12 <= f.size()) {
        const size_t len = be32(&f[i]);
        if (i + 12 + len > f.size()) break;
        const char* type = reinterpret_cast<const char*>(&f[i + 4]);
        const uint8_t* data = &f[i + 8];
        if (std::memcmp(type, "IDAT", 4) == 0) break;
        if (std::memcmp(type, "iCCP", 4) == 0) {
            const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(data, 0, std::min<size_t>(len, 80)));
            if (!nul || nul + 2 > data + len || nul[1] != 0) return {};
            const uint8_t* z = nul + 2;
            const size_t zn = size_t(data + len - z);
            // Profiles are rarely over a few hundred KB; grow until it fits.
            for (uLongf cap = 1 << 16; cap <= (64u << 20); cap *= 4) {
                Bytes out(cap);
                uLongf n = cap;
                const int r = uncompress(out.data(), &n, z, uLong(zn));
                if (r == Z_OK) {
                    out.resize(n);
                    return out;
                }
                if (r != Z_BUF_ERROR) return {};
            }
            return {};
        }
        i += 12 + len;
    }
    return {};
}

bool readCurve(const Bytes& p, size_t off, size_t len, Curve& c) {
    if (len < 12 || off + len > p.size()) return false;
    const uint8_t* t = &p[off];
    if (std::memcmp(t, "curv", 4) == 0) {
        const uint32_t n = be32(t + 8);
        if (12 + size_t(n) * 2 > len) return false;
        c = Curve{};
        if (n == 1) {
            c.type = 0;
            c.g = be16(t + 12) / 256.0f;
        } else {
            c.type = -1;
            for (uint32_t k = 0; k < n; ++k) c.table.push_back(be16(t + 12 + k * 2) / 65535.0f);
        }
        return true;
    }
    if (std::memcmp(t, "para", 4) == 0) {
        static const int kParams[] = {1, 3, 4, 5, 7};
        const int type = be16(t + 8);
        if (type < 0 || type > 4 || 12 + size_t(kParams[type]) * 4 > len) return false;
        float v[7] = {1, 1, 0, 0, 0, 0, 0};
        for (int k = 0; k < kParams[type]; ++k) v[k] = float(s15(t + 12 + k * 4));
        c = Curve{};
        c.type = type;
        c.g = v[0], c.a = v[1], c.b = v[2], c.c = v[3], c.d = v[4], c.e = v[5], c.f = v[6];
        return true;
    }
    return false;
}

std::string readName(const Bytes& p, size_t off, size_t len) {
    if (len < 12 || off + len > p.size()) return {};
    const uint8_t* t = &p[off];
    std::string s;
    if (std::memcmp(t, "desc", 4) == 0) {  // v2: ASCII
        const size_t n = std::min<size_t>(be32(t + 8), len - 12);
        s.assign(reinterpret_cast<const char*>(t + 12), n);
    } else if (std::memcmp(t, "mluc", 4) == 0 && len >= 28) {  // v4: UTF-16BE; keep the ASCII
        const size_t n = be32(t + 20), o = be32(t + 24);
        for (size_t k = 0; k + 1 < n && o + k + 1 < len; k += 2) {
            const uint16_t ch = be16(t + o + k);
            s.push_back(ch < 128 ? char(ch) : '?');
        }
    }
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

double srgbDecode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

}  // namespace

float Curve::eval(float x) const {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (type) {
        case -1: {
            if (table.empty()) return x;
            if (table.size() == 1) return table[0];
            const float pos = x * float(table.size() - 1);
            const size_t i = std::min(size_t(pos), table.size() - 2);
            const float t = pos - float(i);
            return table[i] + (table[i + 1] - table[i]) * t;
        }
        case 0: return std::pow(x, g);
        case 1: return x >= -b / a ? std::pow(std::max(a * x + b, 0.0f), g) : 0.0f;
        case 2: return x >= -b / a ? std::pow(std::max(a * x + b, 0.0f), g) + c : c;
        case 3: return x >= d ? std::pow(std::max(a * x + b, 0.0f), g) : c * x;
        case 4: return x >= d ? std::pow(std::max(a * x + b, 0.0f), g) + e : c * x + f;
    }
    return x;
}

std::vector<uint8_t> embeddedProfile(const std::string& pathU8) {
    // WebP, JPEG XL and AVIF: their decoders find it (or build it from CICP), from the whole file.
    {
        std::ifstream s(u8ToPath(pathU8), std::ios::binary);
        uint8_t head[16] = {};
        s.read(reinterpret_cast<char*>(head), sizeof head);
        if (codecs::sniff(head, size_t(s.gcount())) != codecs::Kind::None) {
            std::vector<char> file;
            if (!readFileBytes(pathU8, file)) return {};
            return codecs::profile(reinterpret_cast<const uint8_t*>(file.data()), file.size());
        }
    }
    const Bytes f = readProfileSegments(pathU8);
    if (f.size() >= 2 && ((f[0] == 'I' && f[1] == 'I') || (f[0] == 'M' && f[1] == 'M'))) {
        // TIFF: the directory's ICC tag. Float TIFFs are scene-linear data, so theirs isn't used.
        tiffdec::Info info;
        std::string err;
        if (tiffdec::probeFile(pathU8, info, err) && !info.isFloat) return info.icc;
        return {};
    }
    if (f.size() > 3 && f[0] == 0xFF && f[1] == 0xD8) return jpegProfile(f);
    if (f.size() > 8 && std::memcmp(f.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return pngProfile(f);
    return {};
}

bool parse(const std::vector<uint8_t>& p, Profile& out, std::string* why) {
    auto fail = [&](const char* r) {
        if (why) *why = r;
        return false;
    };
    if (p.size() < 132 || std::memcmp(&p[36], "acsp", 4) != 0) return fail("not an ICC profile");
    const bool rgb = std::memcmp(&p[16], "RGB ", 4) == 0, gray = std::memcmp(&p[16], "GRAY", 4) == 0;
    if (!rgb && !gray) return fail("not an RGB or grey profile");
    if (std::memcmp(&p[20], "XYZ ", 4) != 0) return fail("Lab connection space");
    const uint32_t count = be32(&p[128]);
    if (132 + size_t(count) * 12 > p.size()) return fail("truncated");
    struct Tag {
        size_t off = 0, len = 0;
    };
    auto find = [&](const char* sig) {
        for (uint32_t k = 0; k < count; ++k) {
            const uint8_t* e = &p[132 + k * 12];
            if (std::memcmp(e, sig, 4) == 0) return Tag{be32(e + 4), be32(e + 8)};
        }
        return Tag{};
    };
    Profile r;
    if (Tag d = find("desc"); d.len) r.name = readName(p, d.off, d.len);
    if (gray) {
        const Tag k = find("kTRC");
        if (!k.len || !readCurve(p, k.off, k.len, r.trc[0])) return fail("no grey curve");
        r.gray = true;
        r.trc[1] = r.trc[2] = r.trc[0];
        out = std::move(r);
        return true;
    }
    static const char* kXyz[] = {"rXYZ", "gXYZ", "bXYZ"};
    static const char* kTrc[] = {"rTRC", "gTRC", "bTRC"};
    double m[3][3];  // columns: the colorants in XYZ (D50)
    for (int c = 0; c < 3; ++c) {
        const Tag x = find(kXyz[c]), t = find(kTrc[c]);
        if (!x.len || !t.len) return fail("LUT-based profile (no matrix)");
        if (x.len < 20 || x.off + 20 > p.size() || std::memcmp(&p[x.off], "XYZ ", 4) != 0) return fail("bad colorant");
        for (int k = 0; k < 3; ++k) m[k][c] = s15(&p[x.off + 8 + k * 4]);
        if (!readCurve(p, t.off, t.len, r.trc[c])) return fail("unsupported curve");
    }
    // XYZ (D50, the connection space) -> D65 with Bradford, as the profile's colorants were
    // adapted from D65 to D50 with it; then XYZ -> linear Rec.709.
    static const double kD50ToD65[3][3] = {{0.9555766, -0.0230393, 0.0631636},
                                           {-0.0282895, 1.0099416, 0.0210077},
                                           {0.0122982, -0.0204830, 1.3299098}};
    static const double kXyzTo709[3][3] = {{3.2404542, -1.5371385, -0.4985314},
                                           {-0.9692660, 1.8760108, 0.0415560},
                                           {0.0556434, -0.2040259, 1.0572252}};
    double a[3][3] = {}, t[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) a[i][j] += kD50ToD65[i][k] * m[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) t[i][j] += kXyzTo709[i][k] * a[k][j];
    // Profile white -> exactly (1, 1, 1): fixed-point colorants and slightly different adaptation
    // matrices would otherwise leave a faint tint on neutrals.
    for (int i = 0; i < 3; ++i) {
        const double w = t[i][0] + t[i][1] + t[i][2];
        if (!(w > 0.5 && w < 2.0)) return fail("colorants don't add up to white");
        for (int j = 0; j < 3; ++j) r.toRec709[i][j] = float(t[i][j] / w);
    }
    out = std::move(r);
    return true;
}

bool isSrgb(const Profile& p) {
    if (p.gray) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::fabs(p.toRec709[i][j] - (i == j ? 1.0f : 0.0f)) > 0.003f) return false;
    for (const Curve& c : p.trc)
        for (int k = 0; k <= 64; ++k) {
            const float x = k / 64.0f;
            if (std::fabs(c.eval(x) - float(srgbDecode(x))) > 0.0025f) return false;
        }
    return true;
}

std::vector<uint8_t> buildMatrixTrc(const std::string& name, const double rgbXyz[3][3], double gamma) {
    auto put32 = [](Bytes& o, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
    };
    auto put16 = [](Bytes& o, uint32_t v) {
        o.push_back(uint8_t(v >> 8));
        o.push_back(uint8_t(v));
    };
    auto s15w = [&](Bytes& o, double v) { put32(o, uint32_t(int32_t(std::lround(v * 65536.0)))); };
    auto xyz = [&](double X, double Y, double Z) {
        Bytes t = {'X', 'Y', 'Z', ' ', 0, 0, 0, 0};
        s15w(t, X);
        s15w(t, Y);
        s15w(t, Z);
        return t;
    };
    Bytes desc = {'d', 'e', 's', 'c', 0, 0, 0, 0};
    put32(desc, uint32_t(name.size() + 1));
    desc.insert(desc.end(), name.begin(), name.end());
    desc.push_back(0);
    desc.resize(desc.size() + 4 + 4 + 2 + 1 + 67, 0);
    Bytes curv = {'c', 'u', 'r', 'v', 0, 0, 0, 0};
    if (gamma > 0) {
        put32(curv, 1);
        put16(curv, uint32_t(std::lround(gamma * 256.0)));
    } else {
        put32(curv, 1024);
        for (int i = 0; i < 1024; ++i) put16(curv, uint32_t(std::lround(srgbDecode(i / 1023.0) * 65535.0)));
    }
    std::vector<std::pair<const char*, Bytes>> tags = {
        {"desc", desc},
        {"wtpt", xyz(0.9642, 1.0, 0.8249)},
        {"rXYZ", xyz(rgbXyz[0][0], rgbXyz[0][1], rgbXyz[0][2])},
        {"gXYZ", xyz(rgbXyz[1][0], rgbXyz[1][1], rgbXyz[1][2])},
        {"bXYZ", xyz(rgbXyz[2][0], rgbXyz[2][1], rgbXyz[2][2])},
        {"rTRC", curv},
        {"gTRC", curv},
        {"bTRC", curv},
    };
    Bytes table, data;
    put32(table, uint32_t(tags.size()));
    const size_t dataStart = 128 + 4 + tags.size() * 12;
    for (auto& [sig, d] : tags) {
        table.insert(table.end(), sig, sig + 4);
        put32(table, uint32_t(dataStart + data.size()));
        put32(table, uint32_t(d.size()));
        data.insert(data.end(), d.begin(), d.end());
        while (data.size() % 4) data.push_back(0);
    }
    Bytes p;
    put32(p, uint32_t(128 + table.size() + data.size()));
    p.insert(p.end(), 4, 0);
    put32(p, 0x02100000);
    for (const char* s : {"mntr", "RGB ", "XYZ "}) p.insert(p.end(), s, s + 4);
    p.insert(p.end(), 12, 0);  // date
    p.insert(p.end(), {'a', 'c', 's', 'p'});
    p.insert(p.end(), 4 + 4 + 4 + 4 + 8 + 4, 0);
    s15w(p, 0.9642);
    s15w(p, 1.0);
    s15w(p, 0.8249);
    p.resize(128, 0);
    p.insert(p.end(), table.begin(), table.end());
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

}  // namespace icc
