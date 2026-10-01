#include "io/RawDecode.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

#include <libraw/libraw.h>

#include "core/Parallel.h"
#include "io/Paths.h"

namespace raw {

bool isRawPath(const std::string& pathU8) {
    std::string e = pathToU8(u8ToPath(pathU8).extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const char* kExts[] = {".cr2", ".cr3", ".crw", ".nef", ".nrw", ".arw", ".srf", ".sr2", ".dng",
                                  ".raf", ".orf", ".rw2", ".pef", ".srw", ".3fr", ".iiq", ".x3f", ".mos",
                                  ".erf", ".kdc", ".mrw", ".raw", ".rwl"};
    for (const char* x : kExts)
        if (e == x) return true;
    return false;
}

std::shared_ptr<Image> load(const std::string& pathU8, std::string& err, int highlights, bool halfSize, int* fullW,
                            int* fullH) {
    // Read the file ourselves: LibRaw's narrow-char open can't take UTF-8 paths on Windows.
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) {
        err = "can't open file";
        return nullptr;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto lr = std::make_unique<LibRaw>();  // large (hundreds of KB): keep it off the stack
    auto fail = [&](int code) {
        err = std::string("RAW: ") + libraw_strerror(code);
        return nullptr;
    };
    if (int r = lr->open_buffer(bytes.data(), bytes.size()); r != LIBRAW_SUCCESS) return fail(r);

    auto& p = lr->imgdata.params;
    p.use_camera_wb = 1;   // "as shot"; Basic's Temperature/Tint are relative to it, like Lightroom's
    p.use_auto_wb = 0;
    p.no_auto_bright = 1;  // keep exposure as shot: brightness is the Develop nodes' job
    p.gamm[0] = p.gamm[1] = 1.0;
    p.output_bps = 16;
    p.half_size = halfSize ? 1 : 0;
    p.highlight = highlights == Blend ? 2 : highlights == Reconstruct ? 5 : 0;
    // Camera RGB out; the camera -> Rec.709 matrix is applied below in float, so colours outside
    // Rec.709 go negative (and get gamut-compressed later) instead of clipping to 0 in 16 bits.
    const bool threeColour = lr->imgdata.idata.colors == 3;
    p.output_color = threeColour ? 0 : 1;

    if (int r = lr->unpack(); r != LIBRAW_SUCCESS) return fail(r);
    if (int r = lr->dcraw_process(); r != LIBRAW_SUCCESS) return fail(r);
    int code = 0;
    libraw_processed_image_t* mem = lr->dcraw_make_mem_image(&code);
    if (!mem) return fail(code);
    std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)> guard(mem, LibRaw::dcraw_clear_mem);
    if (mem->type != LIBRAW_IMAGE_BITMAP || mem->bits != 16 || (mem->colors != 3 && mem->colors != 1)) {
        err = "RAW: unsupported output format";
        return nullptr;
    }

    // With highlight recovery on, LibRaw scales by the largest white-balance multiplier so no
    // channel clips, which leaves the image darker than Clip mode by max/min multiplier. Undo that
    // in float so every mode has the same midtones and recovered highlights simply exceed 1.
    const float* pre = lr->imgdata.color.pre_mul;
    float minMul = 1.0f;
    for (int c = 0; c < 4; ++c)
        if (pre[c] > 0.0f) minMul = std::min(minMul, pre[c]);
    const float gain = 1.0f / 65535.0f / minMul;

    float m[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r][c] = threeColour ? lr->imgdata.color.rgb_cam[r][c] : float(r == c);

    auto img = std::make_shared<Image>(mem->width, mem->height);
    const auto* data = reinterpret_cast<const unsigned short*>(mem->data);
    const int nc = mem->colors;
    parallelFor(img->h, [&](int y) {
        for (int x = 0; x < img->w; ++x) {
            const size_t i = size_t(y) * img->w + x;
            const unsigned short* s = data + i * nc;
            const float cam[3] = {s[0] * gain, s[nc > 1 ? 1 : 0] * gain, s[nc > 1 ? 2 : 0] * gain};
            float* d = img->pixel(i);
            for (int r = 0; r < 3; ++r) d[r] = m[r][0] * cam[0] + m[r][1] * cam[1] + m[r][2] * cam[2];
            d[3] = 1.0f;
        }
    });

    const auto& s = lr->imgdata.sizes;
    const bool swap = (s.flip & 4) != 0;
    if (fullW) *fullW = swap ? s.height : s.width;
    if (fullH) *fullH = swap ? s.width : s.height;
    return img;
}

bool readMetadata(const std::string& pathU8, Metadata& out) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto lr = std::make_unique<LibRaw>();
    if (lr->open_buffer(bytes.data(), bytes.size()) != LIBRAW_SUCCESS) return false;
    const auto& d = lr->imgdata;
    out.make = d.idata.make;
    out.model = d.idata.model;
    out.lens = d.lens.Lens;
    out.exposureTime = d.other.shutter;
    out.fNumber = d.other.aperture;
    out.iso = d.other.iso_speed;
    out.focalLength = d.other.focal_len;
    out.timestamp = static_cast<long long>(d.other.timestamp);
    return true;
}

}  // namespace raw
