#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/Image.h"

// Shared image-processing building blocks for filter / transform / matte nodes.
namespace imageops {

// Bilinear sample at continuous pixel coordinates (pixel centers at +0.5). Outside the image:
// clamp to the edge, or transparent black when `transparentOutside`.
// Inline, as warps call them for every tap of every pixel.
void sampleBilinear(const Image& img, float x, float y, float out[4], bool transparentOutside = false);
float sampleBilinear(const std::vector<float>& ch, int w, int h, float x, float y);

// int(std::floor(v)) without the library call (no SSE4.1 rounding here). Far-off coordinates
// are pulled in first so the conversion can't overflow; they clamp to the edge pixel anyway.
inline int floorToInt(float v) {
    v = std::clamp(v, -1e9f, 1e9f);
    const int i = int(v);
    return i - (float(i) > v);
}

inline void sampleBilinear(const Image& img, float x, float y, float out[4], bool transparentOutside) {
    if (img.empty()) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    if (transparentOutside && (x < 0 || y < 0 || x > img.w || y > img.h)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    x -= 0.5f;
    y -= 0.5f;
    const int x0 = floorToInt(x), y0 = floorToInt(y);
    const float fx = x - x0, fy = y - y0;
    const int xa = std::clamp(x0, 0, img.w - 1), xb = std::clamp(x0 + 1, 0, img.w - 1);
    const int ya = std::clamp(y0, 0, img.h - 1), yb = std::clamp(y0 + 1, 0, img.h - 1);
    const float* ra = img.pixel(size_t(ya) * img.w);
    const float* rb = img.pixel(size_t(yb) * img.w);
    const float *a = ra + size_t(xa) * 4, *b = ra + size_t(xb) * 4, *c = rb + size_t(xa) * 4, *d = rb + size_t(xb) * 4;
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx;
        const float bot = c[k] + (d[k] - c[k]) * fx;
        out[k] = top + (bot - top) * fy;
    }
}

inline float sampleBilinear(const std::vector<float>& ch, int w, int h, float x, float y) {
    x -= 0.5f;
    y -= 0.5f;
    const int x0 = floorToInt(x), y0 = floorToInt(y);
    const float fx = x - x0, fy = y - y0;
    const int xa = std::clamp(x0, 0, w - 1), xb = std::clamp(x0 + 1, 0, w - 1);
    const float* ra = ch.data() + size_t(std::clamp(y0, 0, h - 1)) * w;
    const float* rb = ch.data() + size_t(std::clamp(y0 + 1, 0, h - 1)) * w;
    const float top = ra[xa] + (ra[xb] - ra[xa]) * fx;
    const float bot = rb[xa] + (rb[xb] - rb[xa]) * fx;
    return top + (bot - top) * fy;
}

// Gaussian blur approximated by three box blurs (cost independent of radius). sigma in pixels.
void blurImage(Image& img, float sigmaX, float sigmaY);
void blurChannel(std::vector<float>& ch, int w, int h, float sigmaX, float sigmaY);
// The box radii of those three passes (fewer for tiny sigmas); the GPU blur uses the same.
std::vector<int> boxRadii(float sigma);
// How far (pixels) the blur of a given sigma reads from each pixel: the region padding it needs.
int blurReach(float sigma);

// Euclidean distance (pixels) from every pixel to the nearest pixel where mask != 0.
// Pixels in the mask get 0. Returns +inf-like large values when the mask is empty.
std::vector<float> distanceTransform(const std::vector<uint8_t>& mask, int w, int h);

}  // namespace imageops
