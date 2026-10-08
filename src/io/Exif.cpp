#include "io/Exif.h"

#include <cstdint>
#include <fstream>
#include <vector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "core/Parallel.h"
#include "core/Version.h"
#include "io/Paths.h"
#include "io/RawDecode.h"
#include "io/Tiff.h"

namespace exif {

namespace {

// Reads and patches a TIFF block (the payload of the Exif APP1 segment after "Exif\0\0") in
// either byte order.
struct TiffView {
    uint8_t* t;
    size_t n;
    bool le = true;

    bool valid() {
        if (n < 8) return false;
        le = t[0] == 'I' && t[1] == 'I';
        return le || (t[0] == 'M' && t[1] == 'M');
    }
    unsigned u16(size_t o) const { return le ? t[o] | t[o + 1] << 8 : t[o] << 8 | t[o + 1]; }
    uint32_t u32(size_t o) const {
        return le ? uint32_t(t[o]) | uint32_t(t[o + 1]) << 8 | uint32_t(t[o + 2]) << 16 | uint32_t(t[o + 3]) << 24
                  : uint32_t(t[o]) << 24 | uint32_t(t[o + 1]) << 16 | uint32_t(t[o + 2]) << 8 | uint32_t(t[o + 3]);
    }
    void set16(size_t o, unsigned v) {
        t[o + (le ? 0 : 1)] = uint8_t(v);
        t[o + (le ? 1 : 0)] = uint8_t(v >> 8);
    }
    void set32(size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i) t[o + (le ? i : 3 - i)] = uint8_t(v >> (8 * i));
    }
    // Calls f(entryOffset, tag) for each entry of the IFD at `ifd`; returns the offset of its
    // next-IFD pointer, or 0 when the IFD runs off the block.
    template <class F>
    size_t eachEntry(size_t ifd, F&& f) {
        if (ifd < 8 || ifd + 2 > n) return 0;
        const unsigned count = u16(ifd);
        if (ifd + 2 + size_t(count) * 12 + 4 > n) return 0;
        for (unsigned i = 0; i < count; ++i) {
            const size_t e = ifd + 2 + size_t(i) * 12;
            f(e, u16(e));
        }
        return ifd + 2 + size_t(count) * 12;
    }
};

int tiffOrientation(const uint8_t* t, size_t n) {
    TiffView v{const_cast<uint8_t*>(t), n};
    if (!v.valid()) return 1;
    int o = 1;
    v.eachEntry(v.u32(4), [&](size_t e, unsigned tag) {
        if (tag == 0x0112) {  // Orientation, SHORT
            const unsigned x = v.u16(e + 8);
            o = x >= 1 && x <= 8 ? int(x) : 1;
        }
    });
    return o;
}

// The Exif APP1 payload of a JPEG (after "Exif\0\0"), or empty.
std::vector<uint8_t> jpegExif(const std::string& pathU8) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) return {};
    uint8_t soi[2];
    if (!f.read(reinterpret_cast<char*>(soi), 2) || soi[0] != 0xFF || soi[1] != 0xD8) return {};
    // Walk the segments before the image data; EXIF is in an APP1 near the start.
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t h[4];
        if (!f.read(reinterpret_cast<char*>(h), 4) || h[0] != 0xFF) return {};
        const uint8_t marker = h[1];
        const size_t len = size_t(h[2]) << 8 | h[3];
        if (marker == 0xDA || marker == 0xD9 || len < 2) return {};  // start of scan: no EXIF
        std::vector<uint8_t> seg(len - 2);
        if (!f.read(reinterpret_cast<char*>(seg.data()), std::streamsize(seg.size()))) return {};
        if (marker == 0xE1 && seg.size() > 6 && std::equal(seg.begin(), seg.begin() + 6, "Exif\0\0"))
            return std::vector<uint8_t>(seg.begin() + 6, seg.end());
    }
    return {};
}

bool isJpegPath(const std::string& pathU8) {
    std::string e = pathToU8(u8ToPath(pathU8).extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ".jpg" || e == ".jpeg";
}

// x as a fraction with denominator `den` (EXIF RATIONAL).
void rationalOf(tiff::Ifd& ifd, uint16_t tag, double x, uint32_t den) {
    ifd.rational(tag, uint32_t(std::lround(x * den)), den);
}

std::vector<uint8_t> fromRaw(const raw::Metadata& m, int w, int h) {
    tiff::Ifd ifd0, ex;
    if (!m.make.empty()) ifd0.ascii(0x010F, m.make);
    if (!m.model.empty()) ifd0.ascii(0x0110, m.model);
    ifd0.shorts(0x0112, {1});
    ifd0.ascii(0x0131, std::string("Refractory ") + kRefractoryVersion);
    if (m.timestamp > 0) {
        // LibRaw turns the camera's local "YYYY:MM:DD HH:MM:SS" into a time_t with mktime, so
        // localtime gives the same wall-clock time back.
        std::time_t t = std::time_t(m.timestamp);
        std::tm tm{};
        if (localtime_s(&tm, &t) == 0) {
            char buf[20];
            std::strftime(buf, sizeof buf, "%Y:%m:%d %H:%M:%S", &tm);
            ifd0.ascii(0x0132, buf);  // DateTime
            ex.ascii(0x9003, buf);    // DateTimeOriginal
            ex.ascii(0x9004, buf);    // DateTimeDigitized
        }
    }
    if (m.exposureTime > 0) {
        if (m.exposureTime < 1.0f) ex.rational(0x829A, 1, uint32_t(std::lround(1.0 / m.exposureTime)));
        else rationalOf(ex, 0x829A, m.exposureTime, 10);
    }
    if (m.fNumber > 0) rationalOf(ex, 0x829D, m.fNumber, 10);
    if (m.iso > 0) ex.shorts(0x8827, {uint32_t(std::min(m.iso, 65535.0f))});  // ISOSpeedRatings
    ex.add(0x9000, tiff::Undefined, 4, {'0', '2', '3', '0'});                   // ExifVersion
    if (m.focalLength > 0) rationalOf(ex, 0x920A, m.focalLength, 10);
    ex.shorts(0xA001, {1});  // ColorSpace: sRGB
    ex.longs(0xA002, {uint32_t(w)});
    ex.longs(0xA003, {uint32_t(h)});
    if (!m.lens.empty()) ex.ascii(0xA434, m.lens);  // LensModel

    std::vector<uint8_t> out = tiff::header();
    const uint32_t exOff = ex.write(out);
    ifd0.longs(0x8769, {exOff});  // ExifIFD pointer
    tiff::set32(out, 4, ifd0.write(out));
    return out;
}

// An entry's value as text (ASCII) or a number (SHORT, LONG, RATIONAL; the first value).
std::string asciiAt(const TiffView& v, size_t e) {
    if (v.u16(e + 2) != tiff::Ascii) return {};
    const uint32_t count = v.u32(e + 4);
    const size_t off = count <= 4 ? e + 8 : v.u32(e + 8);
    if (count == 0 || count > 4096 || off + count > v.n) return {};
    std::string s(reinterpret_cast<const char*>(v.t + off), count);
    s.resize(std::strlen(s.c_str()));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

double numberAt(const TiffView& v, size_t e) {
    switch (v.u16(e + 2)) {
        case tiff::Short: return v.u16(e + 8);
        case tiff::Long: return v.u32(e + 8);
        case tiff::Rational: {
            const size_t off = v.u32(e + 8);
            if (off + 8 > v.n) return 0;
            const uint32_t den = v.u32(off + 4);
            return den ? double(v.u32(off)) / den : 0.0;
        }
        default: return 0;
    }
}

std::string captureOf(long long timestamp) {
    if (timestamp <= 0) return {};
    std::time_t t = std::time_t(timestamp);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return {};
    char buf[20];
    std::strftime(buf, sizeof buf, "%Y:%m:%d %H:%M:%S", &tm);
    return buf;
}

// The first `limit` bytes of a file.
std::vector<uint8_t> fileHead(const std::string& pathU8, size_t limit) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> b(limit);
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(limit));
    b.resize(size_t(f.gcount()));
    return b;
}

}  // namespace

bool infoFromTiff(const uint8_t* t, size_t n, PhotoInfo& out) {
    TiffView v{const_cast<uint8_t*>(t), n};
    if (!v.valid()) return false;
    size_t exifIfd = 0;
    std::string dateTime;
    bool any = false;
    v.eachEntry(v.u32(4), [&](size_t e, unsigned tag) {
        if (tag == 0x010F) out.make = asciiAt(v, e);
        else if (tag == 0x0110) out.model = asciiAt(v, e);
        else if (tag == 0x0131) out.software = asciiAt(v, e);
        else if (tag == 0x0132) dateTime = asciiAt(v, e);
        else if (tag == 0x8769) exifIfd = v.u32(e + 8);
        else return;
        any = true;
    });
    v.eachEntry(exifIfd, [&](size_t e, unsigned tag) {
        switch (tag) {
            case 0x829A: out.exposureTime = float(numberAt(v, e)); break;
            case 0x829D: out.fNumber = float(numberAt(v, e)); break;
            case 0x8827: out.iso = float(numberAt(v, e)); break;
            case 0x9003: out.captureTime = asciiAt(v, e); break;
            case 0x920A: out.focalLength = float(numberAt(v, e)); break;
            case 0xA434: out.lens = asciiAt(v, e); break;
            default: return;
        }
        any = true;
    });
    // "0000:00:00 00:00:00" and other placeholders aren't dates.
    auto validDate = [](const std::string& d) { return d.size() >= 10 && d.compare(0, 4, "0000") != 0 && std::isdigit((unsigned char)d[0]); };
    if (!validDate(out.captureTime)) out.captureTime = validDate(dateTime) ? dateTime : std::string();
    return any;
}

bool readInfo(const std::string& pathU8, PhotoInfo& out) {
    out = PhotoInfo{};
    if (isJpegPath(pathU8)) {
        const std::vector<uint8_t> t = jpegExif(pathU8);
        return infoFromTiff(t.data(), t.size(), out);
    }
    // TIFFs and the TIFF-based RAWs (CR2, NEF, ARW, DNG...) keep their EXIF near the start.
    const std::vector<uint8_t> head = fileHead(pathU8, size_t(1) << 20);
    const bool tiffBased = infoFromTiff(head.data(), head.size(), out) && !out.captureTime.empty();
    if (tiffBased || !raw::isRawPath(pathU8)) return tiffBased || !out.make.empty();
    raw::Metadata m;
    if (!raw::readMetadata(pathU8, m)) return false;
    out.make = m.make, out.model = m.model, out.lens = m.lens;
    out.exposureTime = m.exposureTime, out.fNumber = m.fNumber, out.iso = m.iso, out.focalLength = m.focalLength;
    out.captureTime = captureOf(m.timestamp);
    return true;
}

int jpegOrientation(const std::string& pathU8) {
    const std::vector<uint8_t> t = jpegExif(pathU8);
    return tiffOrientation(t.data(), t.size());
}

std::vector<uint8_t> exportBlock(const std::string& sourceU8, int w, int h) {
    if (sourceU8.empty()) return {};
    if (raw::isRawPath(sourceU8)) {
        raw::Metadata m;
        return raw::readMetadata(sourceU8, m) ? fromRaw(m, w, h) : std::vector<uint8_t>{};
    }
    if (!isJpegPath(sourceU8)) return {};
    std::vector<uint8_t> t = jpegExif(sourceU8);
    TiffView v{t.data(), t.size()};
    if (!v.valid()) return {};
    size_t exifIfd = 0;
    const size_t next = v.eachEntry(v.u32(4), [&](size_t e, unsigned tag) {
        if (tag == 0x0112 && v.u16(e + 2) == tiff::Short) v.set16(e + 8, 1);  // upright
        if (tag == 0x8769) exifIfd = v.u32(e + 8);
    });
    if (!next) return {};
    v.set32(next, 0);  // unlink IFD1, the thumbnail
    v.eachEntry(exifIfd, [&](size_t e, unsigned tag) {
        if (tag != 0xA002 && tag != 0xA003) return;  // PixelXDimension / PixelYDimension
        const uint32_t val = uint32_t(tag == 0xA002 ? w : h);
        if (v.u16(e + 2) == tiff::Long) v.set32(e + 8, val);
        else if (v.u16(e + 2) == tiff::Short && val <= 0xFFFF) v.set16(e + 8, val);
    });
    return t;
}

std::shared_ptr<Image> applyOrientation(const std::shared_ptr<Image>& img, int o) {
    if (!img || o <= 1 || o > 8) return img;
    const int w = img->w, h = img->h;
    const bool swap = o >= 5;
    auto out = std::make_shared<Image>(swap ? h : w, swap ? w : h);
    const int ow = out->w, oh = out->h;
    // For each output pixel, the stored pixel it shows. Orientation values per the EXIF spec:
    // 2 mirror, 3 rotate 180, 4 flip, 5 transpose, 6 rotate 90 CW, 7 transverse, 8 rotate 90 CCW.
    parallelFor(oh, [&](int y) {
        for (int x = 0; x < ow; ++x) {
            int sx = x, sy = y;
            switch (o) {
                case 2: sx = w - 1 - x; break;
                case 3: sx = w - 1 - x, sy = h - 1 - y; break;
                case 4: sy = h - 1 - y; break;
                case 5: sx = y, sy = x; break;
                case 6: sx = y, sy = h - 1 - x; break;
                case 7: sx = w - 1 - y, sy = h - 1 - x; break;
                case 8: sx = w - 1 - y, sy = x; break;
            }
            const float* s = img->pixel(size_t(sy) * w + sx);
            float* d = out->pixel(size_t(y) * ow + x);
            for (int c = 0; c < 4; ++c) d[c] = s[c];
        }
    });
    return out;
}

}  // namespace exif
