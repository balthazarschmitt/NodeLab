#include "io/RawDecode.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include <libraw/libraw.h>

#include "core/Parallel.h"
#include "io/Exif.h"
#include "io/ImageIO.h"
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

namespace {

// LibRaw with access to the protected parts that speed up decoding: the processing callbacks and
// the output curve.
class Decoder : public LibRaw {
public:
    // Run our highlight reconstruction in place of LibRaw's: the hook runs just before it.
    void hookHighlights() { callbacks.post_interpolate_cb = &Decoder::afterInterpolate; }
    // The 16-bit output curve that dcraw_make_mem_image applies (built the same way).
    const ushort* outputCurve() {
        const auto& o = imgdata.params;
        int white = 0x2000;  // no_auto_bright: LibRaw's histogram search is skipped
        gamma_curve(o.gamm[0], o.gamm[1], 2, int((white << 3) / o.bright));
        return imgdata.color.curve;
    }

private:
    static void afterInterpolate(void* ctx);
};

// LibRaw's recover_highlights (dcraw's "rebuild" highlight modes, -H 3 and up) with its loops
// spread over threads; in LibRaw it runs on one core and takes about a second at 24 MP. Each
// step gives exactly LibRaw's numbers: the rows of the map and the image are independent, and
// a spreading pass only reads cells that were filled before it (cells it fills are marked
// negative until the pass ends), so the order rows are visited in doesn't matter.
void recoverHighlights(LibRaw& lr) {
    auto& d = lr.imgdata;
    const int colors = d.idata.colors;
    const unsigned width = d.sizes.width, height = d.sizes.height;
    ushort(*image)[4] = d.image;
    const unsigned scale = 4u >> lr.get_internal_data_pointer()->internal_output_params.shrink;
    const float grow = float(std::pow(2.0, 4 - d.params.highlight));
    int hsat[4];
    for (int c = 0; c < colors; ++c) hsat[c] = int(32000 * d.color.pre_mul[c]);
    for (int c = 0; c < colors; ++c)
        if (hsat[c] < 1) return;
    int kc = 0;
    for (int c = 1; c < colors; ++c)
        if (d.color.pre_mul[kc] < d.color.pre_mul[c]) kc = c;
    const int high = int(height / scale), wide = int(width / scale);
    std::vector<float> map(size_t(high) * wide);
    // Rows of the map holding a ratio: a spreading pass only touches rows next to one, so the
    // passes cost nothing where nothing clips.
    std::vector<uint8_t> filled(high);
    static const signed char dir[8][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}};
    for (int c = 0; c < colors; ++c) {
        if (c == kc) continue;
        // Blocks that clip in channel c entirely: the ratio of c to the key channel.
        parallelFor(high, [&](int mrow) {
            for (int mcol = 0; mcol < wide; ++mcol) {
                float sum = 0, wgt = 0;
                unsigned count = 0;
                for (unsigned row = mrow * scale; row < (mrow + 1) * scale; ++row)
                    for (unsigned col = mcol * scale; col < (mcol + 1) * scale; ++col) {
                        const ushort* px = image[row * width + col];
                        if (px[c] >= hsat[c] && px[c] < 2 * hsat[c] && px[kc] > 24000) {  // px[c] / hsat[c] == 1
                            sum += px[c];
                            wgt += px[kc];
                            ++count;
                        }
                    }
                map[size_t(mrow) * wide + mcol] = count == scale * scale ? sum / wgt : 0.0f;
            }
            filled[mrow] = std::any_of(&map[size_t(mrow) * wide], &map[size_t(mrow + 1) * wide], [](float v) { return v > 0; });
        });
        // Spread the ratios into the blocks around them.
        auto nearRatio = [&](int mrow) {
            return filled[mrow] || (mrow > 0 && filled[mrow - 1]) || (mrow + 1 < high && filled[mrow + 1]);
        };
        for (int spread = int(32 / grow); spread--;) {
            parallelFor(high, [&](int mrow) {
                if (!nearRatio(mrow)) return;
                for (int mcol = 0; mcol < wide; ++mcol) {
                    float& m = map[size_t(mrow) * wide + mcol];
                    if (m) continue;
                    float sum = 0;
                    int count = 0;
                    for (int k = 0; k < 8; ++k) {
                        const int y = mrow + dir[k][0], x = mcol + dir[k][1];
                        if (y >= 0 && y < high && x >= 0 && x < wide && map[size_t(y) * wide + x] > 0) {
                            sum += (1 + (k & 1)) * map[size_t(y) * wide + x];
                            count += 1 + (k & 1);
                        }
                    }
                    if (count > 3) m = -(sum + grow) / (count + grow);
                }
            });
            // Rows gaining ratios are only marked now, after the pass has read its neighbours.
            std::atomic<bool> change = false;
            std::vector<uint8_t> gained(high);
            parallelFor(high, [&](int mrow) {
                if (!nearRatio(mrow)) return;
                bool changed = false;
                for (size_t i = size_t(mrow) * wide; i < size_t(mrow + 1) * wide; ++i)
                    if (map[i] < 0) {
                        map[i] = -map[i];
                        changed = true;
                    }
                if (changed) gained[mrow] = 1, change = true;
            });
            for (int r = 0; r < high; ++r) filled[r] |= gained[r];
            if (!change) break;
        }
        // Rebuild the clipped channel from the key channel and the ratio.
        parallelFor(high, [&](int mrow) {
            for (int mcol = 0; mcol < wide; ++mcol) {
                const float m = map[size_t(mrow) * wide + mcol] == 0 ? 1.0f : map[size_t(mrow) * wide + mcol];
                for (unsigned row = mrow * scale; row < (mrow + 1) * scale; ++row)
                    for (unsigned col = mcol * scale; col < (mcol + 1) * scale; ++col) {
                        ushort* px = image[row * width + col];
                        if (px[c] >= 2 * hsat[c]) {  // px[c] / hsat[c] > 1
                            const int val = int(px[kc] * m);
                            if (px[c] < val) px[c] = ushort(std::clamp(val, 0, 65535));
                        }
                    }
            }
        });
    }
}

void Decoder::afterInterpolate(void* ctx) {
    auto* lr = static_cast<LibRaw*>(ctx);
    auto& o = lr->imgdata.params;
    // The callback replaces LibRaw's median filter, which we never ask for (med_passes is 0).
    if (o.highlight > 2) {
        recoverHighlights(*lr);
        o.highlight = 0;  // done: LibRaw's own pass would follow
    }
}

}  // namespace

int repairEdgeLines(uint16_t* raw, size_t pitch, int left, int top, int width, int height, int black, int white) {
    if (!raw || width < 16 || height < 16) return 0;
    const double range = std::max(white - black, 1);
    // An edge line as a run of samples: the k-th row or column in from one of the four edges.
    struct Line {
        uint16_t* p;
        ptrdiff_t step;  // between samples along the line
        int n;
    };
    auto line = [&](int edge, int k) -> Line {
        const ptrdiff_t row = ptrdiff_t(pitch);
        uint16_t* org = raw + size_t(top) * pitch + left;
        switch (edge) {
            case 0: return {org + k * row, 1, width};                         // top
            case 1: return {org + (height - 1 - k) * row, 1, width};          // bottom
            case 2: return {org + k, row, height};                            // left
            default: return {org + (width - 1 - k), row, height};             // right
        }
    };
    // The mean of each of the two colours alternating along a line (lines two apart share them).
    auto means = [](const Line& l, double m[2]) {
        double s[2] = {0, 0};
        for (int i = 0; i < l.n; ++i) s[i & 1] += l.p[i * l.step];
        m[0] = s[0] / ((l.n + 1) / 2);
        m[1] = s[1] / (l.n / 2);
    };
    int repaired = 0;
    for (int edge = 0; edge < 4; ++edge) {
        // Judge both depths before changing anything, each against the two lines behind it: a
        // junk line differs from the next line of its colours by much more than that one differs
        // from the line after, which a real picture's gradient doesn't do over a whole edge. The
        // jump is judged against the signal above black too: the 70D's junk row reads near white
        // in a bright photo but only about twice the signal in a night shot.
        bool junk[2] = {false, false};
        for (int k = 0; k < 2; ++k) {
            double a[2], b[2], c[2];
            means(line(edge, k), a);
            means(line(edge, k + 2), b);
            means(line(edge, k + 4), c);
            for (int i = 0; i < 2; ++i) {
                const double jump = std::abs(a[i] - b[i]), slope = std::abs(b[i] - c[i]);
                if (jump > 0.25 * std::max(b[i] - black, 0.0) && jump > 6.0 * slope + 0.002 * range) junk[k] = true;
            }
        }
        for (int k = 1; k >= 0; --k) {
            if (!junk[k]) continue;
            const Line dst = line(edge, k), src = line(edge, k + 2);
            for (int i = 0; i < dst.n; ++i) dst.p[i * dst.step] = src.p[i * src.step];
            ++repaired;
        }
    }
    return repaired;
}

std::shared_ptr<Image> load(const std::string& pathU8, std::string& err, int highlights, bool halfSize, int* fullW,
                            int* fullH) {
    // Read the file ourselves: LibRaw's narrow-char open can't take UTF-8 paths on Windows.
    std::vector<char> bytes;
    if (!readFileBytes(pathU8, bytes)) {
        err = "can't open file";
        return nullptr;
    }

    auto lr = std::make_unique<Decoder>();  // large (hundreds of KB): keep it off the stack
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

    lr->hookHighlights();
    if (int r = lr->unpack(); r != LIBRAW_SUCCESS) return fail(r);
    // The sensor's size before half_size halves it (rounding up), for the edge fix below.
    const int sensorW = lr->imgdata.sizes.width, sensorH = lr->imgdata.sizes.height;
    // Bayer sensors only (filters below 1000 are LibRaw's codes for X-Trans and other layouts,
    // whose colours don't repeat every two lines).
    if (auto& rd = lr->imgdata.rawdata; rd.raw_image && lr->imgdata.idata.filters >= 1000) {
        const auto& z = lr->imgdata.sizes;
        if (z.left_margin + sensorW <= z.raw_width && z.top_margin + sensorH <= z.raw_height)
            repairEdgeLines(rd.raw_image, z.raw_pitch / 2, z.left_margin, z.top_margin, sensorW, sensorH,
                            int(lr->imgdata.color.black), int(lr->imgdata.color.maximum));
    }
    if (int r = lr->dcraw_process(); r != LIBRAW_SUCCESS) return fail(r);

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

    // Read LibRaw's image directly, through the output curve and the orientation, as
    // dcraw_make_mem_image would (on one core, into a 16-bit copy), but in parallel and to float.
    const auto& s = lr->imgdata.sizes;
    const int nc = lr->imgdata.idata.colors;
    int memW = 0, memH = 0, memColors = 0, memBps = 0;
    lr->get_mem_image_format(&memW, &memH, &memColors, &memBps);
    const bool swap = (s.flip & 4) != 0;
    const int srcW = s.width, srcH = s.height;
    if ((nc != 3 && nc != 1) || memColors != nc || memW != (swap ? srcH : srcW) || memH != (swap ? srcW : srcH)) {
        err = "RAW: unsupported output format";
        return nullptr;
    }
    const ushort* curve = lr->outputCurve();
    const ushort(*image)[4] = lr->imgdata.image;
    // A half-size decode packs each 2x2 Bayer block into one pixel. With an odd sensor width or
    // height (CR3s are 6000x4000 less one), the last column or row of blocks has only one
    // sensor column or row, so a colour is missing and that edge comes out green or yellow.
    // Read the neighbouring complete block there instead.
    const int lastCol = srcW < sensorW && (sensorW & 1) && srcW > 1 ? srcW - 1 : -1;
    const int lastRow = srcH < sensorH && (sensorH & 1) && srcH > 1 ? srcH - 1 : -1;
    auto img = std::make_shared<Image>(memW, memH);
    parallelFor(img->h, [&](int y) {
        for (int x = 0; x < img->w; ++x) {
            int row = y, col = x;  // LibRaw's flip_index
            if (swap) std::swap(row, col);
            if (s.flip & 2) row = srcH - 1 - row;
            if (s.flip & 1) col = srcW - 1 - col;
            if (col == lastCol) --col;
            if (row == lastRow) --row;
            const ushort* src = image[size_t(row) * srcW + col];
            const float cam[3] = {curve[src[0]] * gain, curve[src[nc > 1 ? 1 : 0]] * gain, curve[src[nc > 1 ? 2 : 0]] * gain};
            float* d = img->pixel(size_t(y) * img->w + x);
            for (int r = 0; r < 3; ++r) d[r] = m[r][0] * cam[0] + m[r][1] * cam[1] + m[r][2] * cam[2];
            d[3] = 1.0f;
        }
    });

    // The full-resolution size, also for a half-size decode: previews scale their pixel sizes
    // (blur radii) by proxy / full, so reporting the halved size made them twice the export's.
    const int fw = halfSize ? sensorW : s.width, fh = halfSize ? sensorH : s.height;
    if (fullW) *fullW = swap ? fh : fw;
    if (fullH) *fullH = swap ? fw : fh;
    return img;
}

namespace {

// LibRaw doesn't keep the EXIF ExposureBiasValue, so catch it as its EXIF parser passes by.
void exifTag(void* context, int tag, int type, int len, unsigned int order, void* ifp, INT64) {
    if (tag != 0x9204 || type != 10 || len < 1) return;  // ExposureBiasValue, an SRATIONAL
    unsigned char b[8];
    if (static_cast<LibRaw_abstract_datastream*>(ifp)->read(b, 1, 8) != 8) return;
    auto i32 = [&](const unsigned char* p) {
        const uint32_t u = order == 0x4949 ? uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24
                                           : uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
        return int32_t(u);
    };
    const int32_t num = i32(b), den = i32(b + 4);
    if (den != 0) *static_cast<float*>(context) = float(num) / float(den);
}

}  // namespace

std::shared_ptr<Image> loadThumbnail(const std::string& pathU8, int minEdge, std::string& err) {
    std::vector<char> bytes;
    if (!readFileBytes(pathU8, bytes)) {
        err = "can't open file";
        return nullptr;
    }
    auto lr = std::make_unique<LibRaw>();
    if (int r = lr->open_buffer(bytes.data(), bytes.size()); r != LIBRAW_SUCCESS) {
        err = std::string("RAW: ") + libraw_strerror(r);
        return nullptr;
    }
    // Cameras embed several previews (CR3: 160 px, a 1620 px one and a full-size JPEG); decoding
    // the full-size one would take as long as a RAW decode's demosaic.
    const auto& list = lr->imgdata.thumbs_list;
    int pick = -1, pickEdge = 0;
    for (int i = 0; i < std::min(list.thumbcount, int(LIBRAW_THUMBNAIL_MAXCOUNT)); ++i) {
        const auto& t = list.thumblist[i];
        if (t.tformat != LIBRAW_INTERNAL_THUMBNAIL_JPEG) continue;
        const int edge = std::max(t.twidth, t.theight);
        const bool better = pick < 0 || (pickEdge < minEdge ? edge > pickEdge : edge >= minEdge && edge < pickEdge);
        if (better) pick = i, pickEdge = edge;
    }
    const int r = pick >= 0 ? lr->unpack_thumb_ex(pick) : lr->unpack_thumb();
    if (r != LIBRAW_SUCCESS || lr->imgdata.thumbnail.tformat != LIBRAW_THUMBNAIL_JPEG) {
        err = "RAW: no JPEG preview";
        return nullptr;
    }
    const auto& th = lr->imgdata.thumbnail;
    auto img = decodeImageMemory(reinterpret_cast<const unsigned char*>(th.thumb), th.tlength, err);
    if (!img) return nullptr;
    // The previews are stored as the sensor is; LibRaw's flip is the camera's orientation.
    const int flip = lr->imgdata.sizes.flip;
    return exif::applyOrientation(img, flip == 3 ? 3 : flip == 5 ? 8 : flip == 6 ? 6 : 1);
}

bool readMetadata(const std::string& pathU8, Metadata& out) {
    std::vector<char> bytes;
    if (!readFileBytes(pathU8, bytes)) return false;
    auto lr = std::make_unique<LibRaw>();
    float bias = 0.0f;
    lr->set_exifparser_handler(exifTag, &bias);
    if (lr->open_buffer(bytes.data(), bytes.size()) != LIBRAW_SUCCESS) return false;
    out.exposureBias = std::clamp(bias, -10.0f, 10.0f);  // a corrupt tag shouldn't blow the image out
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

float exposureBias(const std::string& pathU8) {
    // Image Input asks on every evaluation; reading the file once per path is enough.
    static std::mutex mutex;
    static std::map<std::string, float> cache;
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(pathU8); it != cache.end()) return it->second;
    }
    Metadata m;
    const float bias = readMetadata(pathU8, m) ? m.exposureBias : 0.0f;
    std::lock_guard lock(mutex);
    cache[pathU8] = bias;
    return bias;
}

}  // namespace raw
