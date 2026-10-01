#pragma once
// Camera RAW decoding through LibRaw.
#include <memory>
#include <string>

#include "core/Image.h"

namespace raw {

// Highlight Reconstruction choices (Image Input's param order).
enum Highlights { Clip = 0, Blend = 1, Reconstruct = 2 };

// True for the camera RAW extensions LibRaw is offered (CR2, CR3, NEF, ARW, DNG, RAF, ...).
bool isRawPath(const std::string& pathU8);

// Decodes a RAW file to scene-linear Rec.709 (sRGB primaries, D65), white balanced "as shot",
// oriented upright. 1.0 is the sensor's clipping point for the least-sensitive channel; Blend and
// Reconstruct keep partly clipped highlights above 1. halfSize skips demosaicing for a fast
// half-resolution preview. fullW/fullH receive the full-resolution upright size.
std::shared_ptr<Image> load(const std::string& pathU8, std::string& err, int highlights, bool halfSize,
                            int* fullW = nullptr, int* fullH = nullptr);

// Capture settings, for the EXIF of exported JPEGs. Zero / empty where the file has none.
struct Metadata {
    std::string make, model, lens;
    float exposureTime = 0, fNumber = 0, iso = 0, focalLength = 0;  // seconds, f/, ISO, mm
    float exposureBias = 0;                                          // the camera's exposure compensation, EV
    long long timestamp = 0;                                         // capture time (time_t)
};
// Reads only the metadata (no decoding). False when the file can't be opened as a RAW.
bool readMetadata(const std::string& pathU8, Metadata& out);
// Metadata::exposureBias of a RAW file, read once per path; 0 when it has none or can't be read.
float exposureBias(const std::string& pathU8);

}  // namespace raw
