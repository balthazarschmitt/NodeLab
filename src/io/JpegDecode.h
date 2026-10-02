#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

// stb_image's JPEG decoder, run on several threads. It gives exactly the RGBA codes of stb's
// 4-channel load, with the same code underneath (this file's .cpp includes a private copy of
// stb_image), but:
//   - baseline scans with restart markers decode their restart intervals in parallel. Each interval
//     starts from a fixed state, so it can be decoded alone; the result is checked against where
//     the serial decoder would have ended each interval.
//   - a progressive file's dequantize and IDCT pass runs in parallel.
//   - upsampling and colour conversion run per row, on whichever thread converts that row.
// Anything unexpected (a damaged stream, a scan that stops early) returns false, and the caller
// falls back to plain stb_image, so its results and errors stay the same.
namespace jpeg {

class Decoded {
public:
    Decoded();
    ~Decoded();
    Decoded(const Decoded&) = delete;
    Decoded& operator=(const Decoded&) = delete;

    int w = 0, h = 0;

    // Row y's RGBA codes (w * 4). Safe to call from several threads at once.
    void expandRow(int y, uint8_t* rgba) const;

    struct State;
    std::unique_ptr<State> state;
};

bool decode(const uint8_t* file, size_t size, Decoded& out);

}  // namespace jpeg
