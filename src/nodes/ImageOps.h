#pragma once
#include <cstdint>
#include <vector>

#include "core/Image.h"

// Shared image-processing building blocks for filter / transform / matte nodes.
namespace imageops {

// Bilinear sample at continuous pixel coordinates (pixel centers at +0.5). Outside the image:
// clamp to the edge, or transparent black when `transparentOutside`.
void sampleBilinear(const Image& img, float x, float y, float out[4], bool transparentOutside = false);
float sampleBilinear(const std::vector<float>& ch, int w, int h, float x, float y);

// Gaussian blur approximated by three box blurs (cost independent of radius). sigma in pixels.
void blurImage(Image& img, float sigmaX, float sigmaY);
void blurChannel(std::vector<float>& ch, int w, int h, float sigmaX, float sigmaY);

// Euclidean distance (pixels) from every pixel to the nearest pixel where mask != 0.
// Pixels in the mask get 0. Returns +inf-like large values when the mask is empty.
std::vector<float> distanceTransform(const std::vector<uint8_t>& mask, int w, int h);

}  // namespace imageops
