#pragma once
// Export colour spaces (Lightroom's Export > Color Space): the RGB spaces display-referred files
// are written in, their primaries, white points and tone curves, and HDR PNG's Rec.2100 PQ.
#include <array>

namespace outspace {

enum Space { sRGB = 0, DisplayP3, AdobeRGB, ProPhoto, Rec2020, Rec2100PQ, kCount };
inline const char* const kNames[] = {"sRGB", "Display P3", "Adobe RGB (1998)", "ProPhoto RGB", "Rec.2020",
                                     "Rec.2100 PQ (HDR)"};

using Mat3 = std::array<std::array<double, 3>, 3>;  // rows

inline bool valid(int s) { return s >= 0 && s < kCount; }
// HDR: scene-linear values above 1 are kept (up to 10,000 nits), not tone mapped.
inline bool isHdr(int s) { return s == Rec2100PQ; }

// Linear Rec.709 (sRGB's primaries, D65) to the space's linear RGB (Bradford-adapted to D50 for
// ProPhoto). Rec.2100 shares Rec.2020's primaries.
const Mat3& fromRec709(int space);
// The space's primaries as XYZ adapted to the D50 profile connection space (an ICC profile's
// rXYZ, gXYZ, bXYZ columns), and its white point.
const Mat3& toXyzD50(int space);

// The space's tone curve: linear 0..1 to encoded, and back. (PQ: see pqEncode.)
float encode(int space, float linear);
double decode(int space, double encoded);

// SMPTE ST 2084 (PQ): absolute luminance in nits (0..10000) to the signal 0..1.
float pqEncode(float nits);
// Rec.2100's reference white: scene-linear 1.0 is shown at this many nits (ITU-R BT.2408).
constexpr float kHdrReferenceWhite = 203.0f;

inline void apply(const Mat3& m, const float in[3], float out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = float(m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2]);
}

// Soft proofing's gamut warning: a scene-linear Rec.709 colour the space can't hold (a negative
// component there). Highlights above white are the clipping warning's. Display.cpp's shader
// mirrors this with the same matrix as floats.
constexpr float kGamutTolerance = 1e-4f;
inline bool outOfGamut(const float m[9], const float rgb[3]) {
    for (int r = 0; r < 3; ++r)
        if (m[r * 3] * rgb[0] + m[r * 3 + 1] * rgb[1] + m[r * 3 + 2] * rgb[2] < -kGamutTolerance) return true;
    return false;
}
// fromRec709 as floats, rows first (for outOfGamut and the shader).
inline void gamutMatrix(int space, float out[9]) {
    const Mat3& m = fromRec709(space);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = float(m[r][c]);
}

}  // namespace outspace
