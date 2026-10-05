#pragma once
// OpenEXR import through tinyexr: the first part's RGBA, or the first layer that has R, G and B
// (Blender's multilayer files name them "ViewLayer.Combined.R"), or one channel as grey.
#include <cstdint>
#include <string>
#include <vector>

namespace exrdec {

// True when the bytes start with OpenEXR's magic number.
bool isExr(const uint8_t* data, size_t len);
// Straight (un-premultiplied) RGBA floats, alpha 1 without an alpha channel. Values are as stored:
// OpenEXR holds scene-linear light.
bool decode(const uint8_t* data, size_t len, int& w, int& h, std::vector<float>& rgba, std::string& err);

}  // namespace exrdec
