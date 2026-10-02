#pragma once
// A little-endian TIFF directory builder, shared by the TIFF writer and the EXIF block for JPEGs
// (EXIF is a TIFF structure).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tiff {

enum Type : uint16_t { Byte = 1, Ascii = 2, Short = 3, Long = 4, Rational = 5, Undefined = 7 };

inline void put16(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v));
    o.push_back(uint8_t(v >> 8));
}
inline void put32(std::vector<uint8_t>& o, uint32_t v) {
    put16(o, v & 0xFFFF);
    put16(o, v >> 16);
}
inline void set32(std::vector<uint8_t>& o, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) o[at + i] = uint8_t(v >> (8 * i));
}

// "II", 42, then the first IFD's offset (0 for now; set it with set32(out, 4, ...)).
inline std::vector<uint8_t> header() { return {'I', 'I', 42, 0, 0, 0, 0, 0}; }

class Ifd {
public:
    void add(uint16_t tag, Type type, uint32_t count, std::vector<uint8_t> data) {
        entries_.push_back({tag, type, count, std::move(data)});
    }
    void shorts(uint16_t tag, const std::vector<uint32_t>& v) {
        std::vector<uint8_t> d;
        for (uint32_t x : v) put16(d, x);
        add(tag, Short, uint32_t(v.size()), std::move(d));
    }
    void longs(uint16_t tag, const std::vector<uint32_t>& v) {
        std::vector<uint8_t> d;
        for (uint32_t x : v) put32(d, x);
        add(tag, Long, uint32_t(v.size()), std::move(d));
    }
    void rational(uint16_t tag, uint32_t num, uint32_t den) {
        std::vector<uint8_t> d;
        put32(d, num);
        put32(d, den);
        add(tag, Rational, 1, std::move(d));
    }
    void ascii(uint16_t tag, const std::string& s) {
        std::vector<uint8_t> d(s.begin(), s.end());
        d.push_back(0);
        const auto count = uint32_t(d.size());  // before the move: argument order is unspecified
        add(tag, Ascii, count, std::move(d));
    }
    void undefined(uint16_t tag, const std::vector<uint8_t>& bytes) { add(tag, Undefined, uint32_t(bytes.size()), bytes); }
    bool empty() const { return entries_.empty(); }

    // Appends the directory (word aligned, entries sorted by tag, no next IFD) and its out-of-line
    // values to `out`, and returns its offset. `out` starts `base` bytes into the file, whose
    // start is the TIFF header (so with base 0, `out` holds the header).
    uint32_t write(std::vector<uint8_t>& out, size_t base = 0) {
        std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        if ((base + out.size()) & 1) out.push_back(0);
        const uint32_t start = uint32_t(base + out.size());
        size_t extra = start + 2 + entries_.size() * 12 + 4;
        put16(out, uint32_t(entries_.size()));
        std::vector<uint8_t> tail;
        for (const Entry& e : entries_) {
            put16(out, e.tag);
            put16(out, e.type);
            put32(out, e.count);
            if (e.data.size() <= 4) {
                std::vector<uint8_t> v = e.data;
                v.resize(4, 0);  // values of 4 bytes or less sit in the entry, left-justified
                out.insert(out.end(), v.begin(), v.end());
            } else {
                put32(out, uint32_t(extra + tail.size()));
                tail.insert(tail.end(), e.data.begin(), e.data.end());
                if (tail.size() & 1) tail.push_back(0);
            }
        }
        put32(out, 0);
        out.insert(out.end(), tail.begin(), tail.end());
        return start;
    }

private:
    struct Entry {
        uint16_t tag;
        Type type;
        uint32_t count;
        std::vector<uint8_t> data;
    };
    std::vector<Entry> entries_;
};

}  // namespace tiff
