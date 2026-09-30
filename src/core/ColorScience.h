#pragma once
// Colour science for the scene-linear develop maths: Oklab/Oklch, the Planckian locus, CAT16
// white balance and gamut compression. RGB here is always linear Rec.709 (sRGB primaries, D65).

namespace colorsci {

using Mat3 = float[3][3];

void mul(const Mat3 m, const float in[3], float out[3]);

// Björn Ottosson's Oklab: L 0..1 for black..white (unbounded above), a/b about -0.4..0.4.
// Perceptually uniform, so scaling chroma keeps hue and lightness.
void rgbToOklab(const float rgb[3], float lab[3]);
void oklabToRgb(const float lab[3], float rgb[3]);
// Oklab hue in degrees (0..360) of an sRGB-encoded colour, e.g. a colour-wheel pick.
float oklabHueOfSrgb(float r, float g, float b);

// Planckian locus in CIE 1960 uv (Krystek's approximation, 1000..15000 K).
void planckianUv(float kelvin, float& u, float& v);

// Lightroom-style relative white balance as a CAT16 adaptation matrix in linear Rec.709.
// temp and tint are -1..1: +temp warms (the light was assumed cooler than D65), +tint goes toward
// magenta (the light was assumed greener). 0, 0 is the identity. The assumed white keeps Y = 1, so
// only colour changes, not brightness.
void whiteBalanceMatrix(float temp, float tint, Mat3 out);
// The linear RGB (Y = 1) of the light whiteBalanceMatrix(temp, tint) neutralises.
void whiteBalanceSource(float temp, float tint, float rgb[3]);

// Pulls a colour with negative channels toward the grey of the same luminance until it fits,
// keeping its hue instead of clipping channels independently.
void compressToGamut(float rgb[3]);

}  // namespace colorsci
