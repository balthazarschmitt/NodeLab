#include "io/JpegDecode.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/Parallel.h"

// A private copy of stb_image's JPEG decoder (ImageIO.cpp has the public one), for its internals.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb_image.h>

namespace jpeg {

struct Decoded::State {
    stbi__context ctx{};
    stbi__jpeg* j = nullptr;
    int decodeN = 0;
    bool isRgb = false;
    struct Comp {
        resample_row_func resample = nullptr;
        int hs = 1, wLores = 0;
        std::vector<int> nearRow, farRow;  // per output row, rows of the component's data
    } comps[4];

    ~State() {
        if (!j) return;
        stbi__free_jpeg_components(j, ctx.img_n, 0);
        std::free(j);
    }
};

namespace {

// The MCUs [first, last) of an interleaved baseline scan: the loop of stb's
// stbi__parse_entropy_coded_data, from any MCU. Returns 0 on a decode error, 1 when the scan
// stopped at a marker that isn't a restart (as stb's loop does), and 2 when it reached `last`.
int decodeMcus(stbi__jpeg* z, long first, long last) {
    STBI_SIMD_ALIGN(short, data[64]);
    for (long m = first; m < last; ++m) {
        const int i = int(m % z->img_mcu_x), j = int(m / z->img_mcu_x);
        for (int k = 0; k < z->scan_n; ++k) {
            const int n = z->order[k];
            for (int y = 0; y < z->img_comp[n].v; ++y) {
                for (int x = 0; x < z->img_comp[n].h; ++x) {
                    const int x2 = (i * z->img_comp[n].h + x) * 8;
                    const int y2 = (j * z->img_comp[n].v + y) * 8;
                    const int ha = z->img_comp[n].ha;
                    if (!stbi__jpeg_decode_block(z, data, z->huff_dc + z->img_comp[n].hd, z->huff_ac + ha,
                                                 z->fast_ac[ha], n, z->dequant[z->img_comp[n].tq]))
                        return 0;
                    z->idct_block_kernel(z->img_comp[n].data + z->img_comp[n].w2 * y2 + x2, z->img_comp[n].w2, data);
                }
            }
        }
        if (--z->todo <= 0) {
            if (z->code_bits < 24) stbi__grow_buffer_unsafe(z);
            if (!STBI__RESTART(z->marker)) return 1;
            stbi__jpeg_reset(z);
        }
    }
    return 2;
}

// One scan, in place of stbi__parse_entropy_coded_data. An interleaved baseline scan with restart
// markers is split at the markers: every interval starts after a reset, so each is decoded from its
// own copy of the decoder, and writes its own MCUs. Returns false where the serial decoder could
// have done something else (a scan that stops early, an error), and the caller gives up.
bool parseScan(stbi__jpeg* z) {
    const long mcus = long(z->img_mcu_x) * z->img_mcu_y;
    const long ri = z->restart_interval;
    const long intervals = ri > 0 ? (mcus + ri - 1) / ri : 1;
    if (z->progressive || z->scan_n == 1 || intervals < 2 || parallel::workerCount() < 2)
        return stbi__parse_entropy_coded_data(z) == 1;

    // Where each interval starts: after the first marker past the previous start, which must be a
    // restart. Fill bytes (0xFF 0xFF) and stuffed zeros (0xFF 0x00) are skipped as the decoder does.
    std::vector<stbi_uc*> starts{z->s->img_buffer};
    starts.reserve(size_t(intervals) + 1);
    stbi_uc* p = z->s->img_buffer;
    stbi_uc* const end = z->s->img_buffer_end;
    while (long(starts.size()) < intervals && p < end) {
        if (*p++ != 0xff) continue;
        while (p < end && *p == 0xff) ++p;
        if (p >= end) break;
        const stbi_uc c = *p++;
        if (c == 0) continue;
        if (!STBI__RESTART(c)) break;
        starts.push_back(p);
    }
    if (long(starts.size()) < intervals) return false;

    // Copies start from this snapshot, because the last interval runs on z itself (so z ends in
    // the state the serial decoder leaves for the markers after the scan).
    const stbi__jpeg tmpl = *z;
    const stbi__context ctx = *z->s;
    std::atomic<bool> ok{true};
    const int chunk = std::max(1, int(intervals / (parallel::workerCount() * 4)));
    parallel::run(int(intervals), chunk, [&](int k0, int k1) {
        stbi__jpeg* copy = static_cast<stbi__jpeg*>(std::malloc(sizeof(stbi__jpeg)));
        if (!copy) {
            ok = false;
            return;
        }
        stbi__context c = ctx;
        for (int k = k0; k < k1 && ok; ++k) {
            const bool last = k == intervals - 1;
            stbi__jpeg* zk = last ? z : copy;
            if (!last) {
                std::memcpy(copy, &tmpl, sizeof(stbi__jpeg));
                copy->s = &c;
            }
            zk->s->img_buffer = starts[size_t(k)];
            stbi__jpeg_reset(zk);
            const long m1 = std::min(mcus, (k + 1) * ri);
            const int r = decodeMcus(zk, k * ri, m1);
            // An interval before the last must end at its restart marker, where the next begins.
            if (last ? r == 0 : (r != 2 || zk->s->img_buffer != starts[size_t(k) + 1])) ok = false;
        }
        std::free(copy);
    });
    return ok;
}

// stbi__jpeg_finish, with the blocks' rows in parallel.
void finish(stbi__jpeg* z) {
    if (!z->progressive) return;
    for (int n = 0; n < z->s->img_n; ++n) {
        const int w = (z->img_comp[n].x + 7) >> 3;
        const int h = (z->img_comp[n].y + 7) >> 3;
        parallelFor(h, [&](int j) {
            for (int i = 0; i < w; ++i) {
                short* data = z->img_comp[n].coeff + 64 * (i + j * z->img_comp[n].coeff_w);
                stbi__jpeg_dequantize(data, z->dequant[z->img_comp[n].tq]);
                z->idct_block_kernel(z->img_comp[n].data + z->img_comp[n].w2 * j * 8 + i * 8, z->img_comp[n].w2, data);
            }
        });
    }
}

// stbi__decode_jpeg_image with the scans and the final pass above.
bool decodeImage(stbi__jpeg* j) {
    for (int m = 0; m < 4; m++) {
        j->img_comp[m].raw_data = nullptr;
        j->img_comp[m].raw_coeff = nullptr;
    }
    j->restart_interval = 0;
    if (!stbi__decode_jpeg_header(j, STBI__SCAN_load)) return false;
    int m = stbi__get_marker(j);
    while (!stbi__EOI(m)) {
        if (stbi__SOS(m)) {
            if (!stbi__process_scan_header(j)) return false;
            if (!parseScan(j)) return false;
            if (j->marker == STBI__MARKER_none) j->marker = stbi__skip_jpeg_junk_at_end(j);
            m = stbi__get_marker(j);
            if (STBI__RESTART(m)) m = stbi__get_marker(j);
        } else if (stbi__DNL(m)) {
            const int ld = stbi__get16be(j->s);
            const stbi__uint32 nl = stbi__get16be(j->s);
            if (ld != 4 || nl != j->s->img_y) return false;
            m = stbi__get_marker(j);
        } else {
            // stb stops here and keeps what it has; leave that rare case to it.
            if (!stbi__process_marker(j, m)) return false;
            m = stbi__get_marker(j);
        }
    }
    finish(j);
    return true;
}

}  // namespace

Decoded::Decoded() = default;
Decoded::~Decoded() = default;

bool decode(const uint8_t* file, size_t size, Decoded& out) {
    if (size < 4 || size > size_t(INT_MAX) || file[0] != 0xff || file[1] != 0xd8) return false;
    auto st = std::make_unique<Decoded::State>();
    stbi__start_mem(&st->ctx, file, int(size));
    st->ctx.img_n = 0;  // so ~State frees nothing if the header fails
    st->j = static_cast<stbi__jpeg*>(std::malloc(sizeof(stbi__jpeg)));
    if (!st->j) return false;
    std::memset(st->j, 0, sizeof(stbi__jpeg));
    stbi__jpeg* z = st->j;
    z->s = &st->ctx;
    stbi__setup_jpeg(z);
    if (!decodeImage(z)) return false;

    // load_jpeg_image's setup for a 4-channel result.
    const int imgN = z->s->img_n;
    st->isRgb = imgN == 3 && (z->rgb == 3 || (z->app14_color_transform == 0 && !z->jfif));
    st->decodeN = imgN;
    if (st->decodeN <= 0) return false;
    const int h = int(z->s->img_y);
    for (int k = 0; k < st->decodeN; ++k) {
        auto& c = st->comps[k];
        const int hs = z->img_h_max / z->img_comp[k].h, vs = z->img_v_max / z->img_comp[k].v;
        c.hs = hs;
        c.wLores = int((z->s->img_x + stbi__uint32(hs) - 1) / stbi__uint32(hs));
        if (hs == 1 && vs == 1) c.resample = resample_row_1;
        else if (hs == 1 && vs == 2) c.resample = stbi__resample_row_v_2;
        else if (hs == 2 && vs == 1) c.resample = stbi__resample_row_h_2;
        else if (hs == 2 && vs == 2) c.resample = z->resample_row_hv_2_kernel;
        else c.resample = stbi__resample_row_generic;
        // stb steps line0/line1 down the component as it outputs rows; the same walk, recorded.
        c.nearRow.resize(size_t(h));
        c.farRow.resize(size_t(h));
        int ystep = vs >> 1, ypos = 0, l0 = 0, l1 = 0;
        for (int y = 0; y < h; ++y) {
            const bool bot = ystep >= (vs >> 1);
            c.nearRow[size_t(y)] = bot ? l1 : l0;
            c.farRow[size_t(y)] = bot ? l0 : l1;
            if (++ystep >= vs) {
                ystep = 0;
                l0 = l1;
                if (++ypos < z->img_comp[k].y) ++l1;
            }
        }
    }
    out.w = int(z->s->img_x);
    out.h = h;
    out.state = std::move(st);
    return true;
}

// load_jpeg_image's per-row loop for n = 4.
void Decoded::expandRow(int y, uint8_t* out) const {
    const State& st = *state;
    const stbi__jpeg* z = st.j;
    const int imgN = st.ctx.img_n;
    thread_local std::vector<stbi_uc> linebuf[4];
    stbi_uc* coutput[4] = {};
    for (int k = 0; k < st.decodeN; ++k) {
        const auto& c = st.comps[k];
        linebuf[k].resize(size_t(w) + 3);
        stbi_uc* base = z->img_comp[k].data;
        const int w2 = z->img_comp[k].w2;
        coutput[k] = c.resample(linebuf[k].data(), base + size_t(c.nearRow[size_t(y)]) * size_t(w2),
                                base + size_t(c.farRow[size_t(y)]) * size_t(w2), c.wLores, c.hs);
    }
    const int n = 4;
    stbi_uc* yy = coutput[0];
    if (imgN == 3) {
        if (st.isRgb) {
            for (int i = 0; i < w; ++i, out += n) out[0] = yy[i], out[1] = coutput[1][i], out[2] = coutput[2][i], out[3] = 255;
        } else {
            z->YCbCr_to_RGB_kernel(out, yy, coutput[1], coutput[2], w, n);
        }
    } else if (imgN == 4) {
        if (z->app14_color_transform == 0) {  // CMYK
            for (int i = 0; i < w; ++i, out += n) {
                const stbi_uc m = coutput[3][i];
                out[0] = stbi__blinn_8x8(coutput[0][i], m);
                out[1] = stbi__blinn_8x8(coutput[1][i], m);
                out[2] = stbi__blinn_8x8(coutput[2][i], m);
                out[3] = 255;
            }
        } else if (z->app14_color_transform == 2) {  // YCCK
            z->YCbCr_to_RGB_kernel(out, yy, coutput[1], coutput[2], w, n);
            for (int i = 0; i < w; ++i, out += n) {
                const stbi_uc m = coutput[3][i];
                out[0] = stbi__blinn_8x8(255 - out[0], m);
                out[1] = stbi__blinn_8x8(255 - out[1], m);
                out[2] = stbi__blinn_8x8(255 - out[2], m);
            }
        } else {
            z->YCbCr_to_RGB_kernel(out, yy, coutput[1], coutput[2], w, n);
        }
    } else {
        for (int i = 0; i < w; ++i, out += n) out[0] = out[1] = out[2] = yy[i], out[3] = 255;
    }
}

}  // namespace jpeg
