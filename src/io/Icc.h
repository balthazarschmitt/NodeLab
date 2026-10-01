#pragma once
// Input colour profiles: the ICC profile embedded in a JPEG or PNG, so photos that aren't sRGB
// (Display P3 from phones, Adobe RGB or ProPhoto from cameras and editors) decode to the right
// linear Rec.709 colours instead of being read as if they were sRGB.
#include <cstdint>
#include <string>
#include <vector>

namespace icc {

// The ICC profile stored in a JPEG (APP2 ICC_PROFILE segments) or PNG (iCCP chunk); empty when
// the file has none or isn't one of those formats.
std::vector<uint8_t> embeddedProfile(const std::string& pathU8);

// A tone response curve (ICC curv or para), from encoded 0..1 to linear 0..1.
struct Curve {
    int type = -1;             // -1 table (empty: identity), 0..4 the ICC parametric function types
    std::vector<float> table;  // curv samples, evenly spaced over 0..1
    float g = 1, a = 1, b = 0, c = 0, d = 0, e = 0, f = 0;
    float eval(float x) const;
};

// A matrix/TRC display profile: what almost every RGB or grey photo profile is.
struct Profile {
    std::string name;  // the profile's description, e.g. "Display P3"
    bool gray = false;  // one curve for every channel, no matrix
    Curve trc[3];
    // Linear profile RGB -> linear Rec.709 (D65), white mapping to white.
    float toRec709[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

// False (with the reason) for profiles this can't apply: LUT-based (A2B0 only), CMYK, Lab, or
// malformed. Callers then read the image as sRGB.
bool parse(const std::vector<uint8_t>& bytes, Profile& out, std::string* why = nullptr);

// True when the profile is sRGB in all but rounding (the many sRGB profiles in the wild differ
// slightly), so the file decodes exactly as an untagged one would.
bool isSrgb(const Profile& p);

// A minimal ICC v2 matrix/TRC RGB profile, for tests and P3/Adobe RGB round trips. rgbXyz holds the
// red, green and blue colorants in the D50 connection space; gamma < 0 means the sRGB curve.
std::vector<uint8_t> buildMatrixTrc(const std::string& name, const double rgbXyz[3][3], double gamma);

}  // namespace icc
