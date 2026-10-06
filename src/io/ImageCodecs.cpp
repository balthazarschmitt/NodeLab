#include "io/ImageCodecs.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>

#include <avif/avif.h>
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>
#include <webp/decode.h>
#include <webp/encode.h>

#include "core/OutputSpace.h"
#include "io/ImageWrite.h"

namespace codecs {

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

int threads() { return int(std::clamp(std::thread::hardware_concurrency(), 1u, 64u)); }

// The chunks of a RIFF WebP file after "WEBP", by type; empty when it isn't one.
std::vector<webp::Chunk> riffChunks(const uint8_t* d, size_t n) {
    std::vector<webp::Chunk> out;
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WEBP", 4) != 0) return out;
    for (size_t at = 12; at + 8 <= n;) {
        const size_t len = le32(d + at + 4);
        if (len > n - at - 8) break;
        out.push_back({std::string(reinterpret_cast<const char*>(d + at), 4), Bytes(d + at + 8, d + at + 8 + len)});
        at += 8 + len + (len & 1);
    }
    return out;
}

struct JxlEncoderFree {
    void operator()(JxlEncoder* e) const { JxlEncoderDestroy(e); }
};
struct JxlDecoderFree {
    void operator()(JxlDecoder* d) const { JxlDecoderDestroy(d); }
};
struct JxlRunnerFree {
    void operator()(void* r) const { JxlThreadParallelRunnerDestroy(r); }
};
struct AvifEncoderFree {
    void operator()(avifEncoder* e) const { avifEncoderDestroy(e); }
};
struct AvifDecoderFree {
    void operator()(avifDecoder* d) const { avifDecoderDestroy(d); }
};
struct AvifImageFree {
    void operator()(avifImage* i) const { avifImageDestroy(i); }
};

// The ICC profile for what an AVIF's CICP names, for the spaces NodeLab exports; empty for sRGB
// and for unknown ones (read as sRGB).
Bytes cicpProfile(int primaries, int transfer) {
    switch (primaries) {
        case AVIF_COLOR_PRIMARIES_SMPTE432: return iccProfile(outspace::DisplayP3);
        case AVIF_COLOR_PRIMARIES_BT2020:
            return iccProfile(transfer == AVIF_TRANSFER_CHARACTERISTICS_PQ ? outspace::Rec2100PQ : outspace::Rec2020);
        default: return {};
    }
}

}  // namespace

// ---------------------------------------------------------------- WebP

bool webpLossy(const Pixels& px, int quality, std::vector<webp::Chunk>& chunks, std::string& err) {
    if (px.sixteen) {
        err = "WebP is 8 bit";
        return false;
    }
    WebPConfig config;
    WebPPicture pic;
    if (!WebPConfigInit(&config) || !WebPPictureInit(&pic)) {
        err = "libwebp version mismatch";
        return false;
    }
    config.quality = float(std::clamp(quality, 1, 100));
    config.method = 4;
    // Sharp RGB to YUV keeps saturated edges (red on blue) from bleeding, as cwebp -sharp_yuv.
    config.use_sharp_yuv = 1;
    config.thread_level = 1;
    pic.use_argb = 0;
    pic.width = px.w;
    pic.height = px.h;
    const auto* rgb = static_cast<const uint8_t*>(px.data);
    const int ok = px.comp == 4 ? WebPPictureImportRGBA(&pic, rgb, px.w * 4) : WebPPictureImportRGB(&pic, rgb, px.w * 3);
    if (!ok) {
        WebPPictureFree(&pic);
        err = "out of memory";
        return false;
    }
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &writer;
    const bool encoded = WebPEncode(&config, &pic);
    const int code = pic.error_code;
    WebPPictureFree(&pic);
    if (!encoded) {
        WebPMemoryWriterClear(&writer);
        err = "WebP encoding failed (" + std::to_string(code) + ")";
        return false;
    }
    // libwebp writes a whole file; keep its image chunks for NodeLab's container, which adds the
    // profile and metadata.
    chunks.clear();
    for (webp::Chunk& c : riffChunks(writer.mem, writer.size))
        if (c.type == "ALPH" || c.type == "VP8 ") chunks.push_back(std::move(c));
    WebPMemoryWriterClear(&writer);
    if (chunks.empty() || chunks.back().type != "VP8 ") {
        err = "unexpected libwebp output";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- JPEG XL

bool encodeJxl(const Pixels& px, int quality, bool lossless, const Tags& tags, Bytes& out, std::string& err) {
    std::unique_ptr<JxlEncoder, JxlEncoderFree> enc(JxlEncoderCreate(nullptr));
    std::unique_ptr<void, JxlRunnerFree> runner(JxlThreadParallelRunnerCreate(nullptr, size_t(threads())));
    if (!enc || !runner) {
        err = "out of memory";
        return false;
    }
    auto fail = [&](const char* what) {
        err = std::string("JPEG XL: ") + what + " failed";
        return false;
    };
    if (JxlEncoderSetParallelRunner(enc.get(), JxlThreadParallelRunner, runner.get()) != JXL_ENC_SUCCESS)
        return fail("threads");
    const bool alpha = px.comp == 4;
    const int bits = px.sixteen ? 16 : 8;
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = uint32_t(px.w);
    info.ysize = uint32_t(px.h);
    info.bits_per_sample = uint32_t(bits);
    info.num_color_channels = 3;
    info.num_extra_channels = alpha ? 1 : 0;
    info.alpha_bits = alpha ? uint32_t(bits) : 0;
    // Lossless keeps the samples in their own space; lossy codes in XYB, converting from it.
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (tags.pq) info.intensity_target = 10000.0f;
    if (JxlEncoderSetBasicInfo(enc.get(), &info) != JXL_ENC_SUCCESS) return fail("header");
    if (tags.pq || tags.icc.empty()) {
        JxlColorEncoding ce;
        JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
        if (tags.pq) {
            ce.primaries = JXL_PRIMARIES_2100;
            ce.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
            ce.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
        }
        if (JxlEncoderSetColorEncoding(enc.get(), &ce) != JXL_ENC_SUCCESS) return fail("colour encoding");
    } else if (JxlEncoderSetICCProfile(enc.get(), tags.icc.data(), tags.icc.size()) != JXL_ENC_SUCCESS) {
        return fail("ICC profile");
    }
    // Metadata boxes go before the codestream, where readers look first.
    if (!tags.exif.empty() || !tags.xmp.empty()) {
        if (JxlEncoderUseBoxes(enc.get()) != JXL_ENC_SUCCESS) return fail("boxes");
        if (!tags.exif.empty()) {
            Bytes box = {0, 0, 0, 0};  // the TIFF header's offset within the box
            box.insert(box.end(), tags.exif.begin(), tags.exif.end());
            if (JxlEncoderAddBox(enc.get(), "Exif", box.data(), box.size(), JXL_FALSE) != JXL_ENC_SUCCESS)
                return fail("EXIF");
        }
        if (!tags.xmp.empty() &&
            JxlEncoderAddBox(enc.get(), "xml ", reinterpret_cast<const uint8_t*>(tags.xmp.data()), tags.xmp.size(),
                             JXL_FALSE) != JXL_ENC_SUCCESS)
            return fail("XMP");
        JxlEncoderCloseBoxes(enc.get());
    }
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (!fs) return fail("settings");
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 7);
    if (lossless) {
        if (JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS) return fail("lossless");
    } else {
        JxlEncoderSetFrameDistance(fs, JxlEncoderDistanceFromQuality(float(std::clamp(quality, 1, 99))));
    }
    const JxlPixelFormat fmt = {uint32_t(px.comp), px.sixteen ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    const size_t size = size_t(px.w) * px.h * px.comp * (px.sixteen ? 2 : 1);
    if (JxlEncoderAddImageFrame(fs, &fmt, px.data, size) != JXL_ENC_SUCCESS) return fail("encoding");
    JxlEncoderCloseInput(enc.get());
    out.assign(std::max<size_t>(size / 8, 65536), 0);
    uint8_t* next = out.data();
    size_t avail = out.size();
    for (;;) {
        const JxlEncoderStatus s = JxlEncoderProcessOutput(enc.get(), &next, &avail);
        if (s == JXL_ENC_SUCCESS) break;
        if (s != JXL_ENC_NEED_MORE_OUTPUT) return fail("encoding");
        const size_t used = size_t(next - out.data());
        out.resize(out.size() * 2);
        next = out.data() + used;
        avail = out.size() - used;
    }
    out.resize(size_t(next - out.data()));
    return true;
}

// ---------------------------------------------------------------- AVIF

bool encodeAvif(const Pixels& px, int quality, bool lossless, const Tags& tags, Bytes& out, std::string& err) {
    const avifPixelFormat yuv = lossless || quality >= 90 ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV420;
    std::unique_ptr<avifImage, AvifImageFree> image(avifImageCreate(uint32_t(px.w), uint32_t(px.h), px.sixteen ? 10 : 8, yuv));
    std::unique_ptr<avifEncoder, AvifEncoderFree> enc(avifEncoderCreate());
    if (!image || !enc) {
        err = "out of memory";
        return false;
    }
    auto fail = [&](const char* what, avifResult r) {
        err = std::string("AVIF: ") + what + " (" + avifResultToString(r) + ")";
        return false;
    };
    // sRGB and PQ are named by CICP; other spaces carry their profile (and CICP says unspecified).
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
    if (tags.pq) {
        image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
        image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_PQ;
    } else if (tags.icc.empty()) {
        image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
        image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    } else if (avifResult r = avifImageSetProfileICC(image.get(), tags.icc.data(), tags.icc.size()); r != AVIF_RESULT_OK) {
        return fail("ICC profile", r);
    }
    // Lossless keeps RGB as is; otherwise BT.601's matrix, what libavif's tools and browsers use.
    image->matrixCoefficients = lossless ? AVIF_MATRIX_COEFFICIENTS_IDENTITY
                                : tags.pq ? AVIF_MATRIX_COEFFICIENTS_BT2020_NCL
                                          : AVIF_MATRIX_COEFFICIENTS_BT601;
    image->yuvRange = AVIF_RANGE_FULL;
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image.get());
    rgb.format = px.comp == 4 ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
    rgb.depth = px.sixteen ? uint32_t(px.bits) : 8;
    rgb.pixels = static_cast<uint8_t*>(const_cast<void*>(px.data));
    rgb.rowBytes = uint32_t(px.w * px.comp * (px.sixteen ? 2 : 1));
    if (avifResult r = avifImageRGBToYUV(image.get(), &rgb); r != AVIF_RESULT_OK) return fail("conversion", r);
    if (!tags.exif.empty())
        if (avifResult r = avifImageSetMetadataExif(image.get(), tags.exif.data(), tags.exif.size()); r != AVIF_RESULT_OK)
            return fail("EXIF", r);
    if (!tags.xmp.empty())
        if (avifResult r = avifImageSetMetadataXMP(image.get(), reinterpret_cast<const uint8_t*>(tags.xmp.data()),
                                                   tags.xmp.size());
            r != AVIF_RESULT_OK)
            return fail("XMP", r);
    // EXIF's orientation would otherwise become irot/imir; exports are already upright.
    image->transformFlags &= ~uint32_t(AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR);
    enc->maxThreads = threads();
    enc->speed = 6;
    enc->quality = lossless ? AVIF_QUALITY_LOSSLESS : std::clamp(quality, 1, 99);
    enc->qualityAlpha = lossless ? AVIF_QUALITY_LOSSLESS : std::clamp(quality, 1, 99);
    avifRWData data = AVIF_DATA_EMPTY;
    if (avifResult r = avifEncoderWrite(enc.get(), image.get(), &data); r != AVIF_RESULT_OK) {
        avifRWDataFree(&data);
        return fail("encoding", r);
    }
    out.assign(data.data, data.data + data.size);
    avifRWDataFree(&data);
    return true;
}

// ---------------------------------------------------------------- Reading

Kind sniff(const uint8_t* d, size_t n) {
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return Kind::WebP;
    // A bare JPEG XL codestream, or the ISO BMFF container's signature box.
    if (n >= 2 && d[0] == 0xFF && d[1] == 0x0A) return Kind::JXL;
    if (n >= 12 && std::memcmp(d, "\0\0\0\x0CJXL \x0D\x0A\x87\x0A", 12) == 0) return Kind::JXL;
    // ISO BMFF with an AVIF brand: ftyp's major brand, or a compatible one.
    if (n >= 16 && std::memcmp(d + 4, "ftyp", 4) == 0) {
        const size_t box = std::min<size_t>(size_t(d[0]) << 24 | size_t(d[1]) << 16 | size_t(d[2]) << 8 | d[3], n);
        for (size_t at = 8; at + 4 <= box; at += 4) {
            if (at == 12) continue;  // the minor version
            if (std::memcmp(d + at, "avif", 4) == 0 || std::memcmp(d + at, "avis", 4) == 0) return Kind::AVIF;
        }
    }
    return Kind::None;
}

namespace {

bool decodeWebp(const uint8_t* d, size_t n, Decoded& out, std::string& err) {
    int w = 0, h = 0;
    uint8_t* px = WebPDecodeRGBA(d, n, &w, &h);
    if (!px) {
        err = "not a readable WebP file";
        return false;
    }
    out.w = w;
    out.h = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < out.rgba.size(); ++i) out.rgba[i] = uint16_t(px[i] * 257);
    WebPFree(px);
    return true;
}

bool decodeJxl(const uint8_t* d, size_t n, Decoded& out, std::string& err) {
    std::unique_ptr<JxlDecoder, JxlDecoderFree> dec(JxlDecoderCreate(nullptr));
    std::unique_ptr<void, JxlRunnerFree> runner(JxlThreadParallelRunnerCreate(nullptr, size_t(threads())));
    if (!dec || !runner) {
        err = "out of memory";
        return false;
    }
    JxlDecoderSetParallelRunner(dec.get(), JxlThreadParallelRunner, runner.get());
    JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
    // Orientation is applied with the other formats' (scene-linear projects only).
    JxlDecoderSetKeepOrientation(dec.get(), JXL_TRUE);
    JxlDecoderSetInput(dec.get(), d, n);
    JxlDecoderCloseInput(dec.get());
    const JxlPixelFormat fmt = {4, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    for (;;) {
        const JxlDecoderStatus s = JxlDecoderProcessInput(dec.get());
        if (s == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) break;
            if (info.xsize == 0 || info.ysize == 0 || uint64_t(info.xsize) * info.ysize > (uint64_t(1) << 30)) {
                err = "JPEG XL image too large";
                return false;
            }
            out.w = int(info.xsize);
            out.h = int(info.ysize);
            out.orientation = std::clamp(int(info.orientation), 1, 8);
        } else if (s == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            out.rgba.assign(size_t(out.w) * out.h * 4, 0);
            size_t need = 0;
            if (JxlDecoderImageOutBufferSize(dec.get(), &fmt, &need) != JXL_DEC_SUCCESS || need != out.rgba.size() * 2 ||
                JxlDecoderSetImageOutBuffer(dec.get(), &fmt, out.rgba.data(), out.rgba.size() * 2) != JXL_DEC_SUCCESS)
                break;
        } else if (s == JXL_DEC_FULL_IMAGE) {
            return !out.rgba.empty();  // the first frame (an animation's others are ignored)
        } else if (s == JXL_DEC_SUCCESS) {
            return !out.rgba.empty();
        } else {
            break;
        }
    }
    err = "not a readable JPEG XL file";
    return false;
}

bool decodeAvif(const uint8_t* d, size_t n, Decoded& out, std::string& err) {
    std::unique_ptr<avifDecoder, AvifDecoderFree> dec(avifDecoderCreate());
    if (!dec) {
        err = "out of memory";
        return false;
    }
    dec->maxThreads = threads();
    // Files from other tools are often a little off the spec; read what can be read.
    dec->strictFlags = AVIF_STRICT_DISABLED;
    dec->ignoreExif = AVIF_TRUE;
    dec->ignoreXMP = AVIF_TRUE;
    auto fail = [&](avifResult r) {
        err = std::string("not a readable AVIF file (") + avifResultToString(r) + ")";
        return false;
    };
    if (avifResult r = avifDecoderSetIOMemory(dec.get(), d, n); r != AVIF_RESULT_OK) return fail(r);
    if (avifResult r = avifDecoderParse(dec.get()); r != AVIF_RESULT_OK) return fail(r);
    if (avifResult r = avifDecoderNextImage(dec.get()); r != AVIF_RESULT_OK) return fail(r);
    const avifImage* im = dec->image;
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, im);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 16;
    out.w = int(im->width);
    out.h = int(im->height);
    out.rgba.assign(size_t(out.w) * out.h * 4, 0);
    rgb.pixels = reinterpret_cast<uint8_t*>(out.rgba.data());
    rgb.rowBytes = uint32_t(out.w * 8);
    rgb.maxThreads = threads();
    if (avifResult r = avifImageYUVToRGB(im, &rgb); r != AVIF_RESULT_OK) return fail(r);
    // irot turns anticlockwise in quarter turns, then imir mirrors (axis 0 top-bottom, 1 left-right),
    // as EXIF orientations: (angle, mirror) to the tag.
    const int angle = im->transformFlags & AVIF_TRANSFORM_IROT ? im->irot.angle & 3 : 0;
    const int mirror = im->transformFlags & AVIF_TRANSFORM_IMIR ? im->imir.axis + 1 : 0;
    static const int kOrientation[4][3] = {
        {1, 4, 2},  // no turn: none, flip vertically, mirror horizontally
        {8, 5, 7},  // 90 anticlockwise
        {3, 2, 4},  // 180
        {6, 7, 5},  // 270 anticlockwise (90 clockwise)
    };
    out.orientation = kOrientation[angle][mirror];
    return true;
}

}  // namespace

bool decode(const uint8_t* d, size_t n, Decoded& out, std::string& err) {
    switch (sniff(d, n)) {
        case Kind::WebP: return decodeWebp(d, n, out, err);
        case Kind::JXL: return decodeJxl(d, n, out, err);
        case Kind::AVIF: return decodeAvif(d, n, out, err);
        default: err = "unknown format"; return false;
    }
}

Bytes profile(const uint8_t* d, size_t n) {
    switch (sniff(d, n)) {
        case Kind::WebP:
            for (const webp::Chunk& c : riffChunks(d, n))
                if (c.type == "ICCP") return c.data;
            return {};
        case Kind::JXL: {
            std::unique_ptr<JxlDecoder, JxlDecoderFree> dec(JxlDecoderCreate(nullptr));
            if (!dec) return {};
            JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_COLOR_ENCODING);
            JxlDecoderSetInput(dec.get(), d, n);
            JxlDecoderCloseInput(dec.get());
            if (JxlDecoderProcessInput(dec.get()) != JXL_DEC_COLOR_ENCODING) return {};
            // The samples' space as decoded (the original for lossless, which keeps it).
            size_t size = 0;
            if (JxlDecoderGetICCProfileSize(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA, &size) != JXL_DEC_SUCCESS || !size)
                return {};
            Bytes icc(size);
            if (JxlDecoderGetColorAsICCProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), size) != JXL_DEC_SUCCESS)
                return {};
            return icc;
        }
        case Kind::AVIF: {
            std::unique_ptr<avifDecoder, AvifDecoderFree> dec(avifDecoderCreate());
            if (!dec) return {};
            dec->strictFlags = AVIF_STRICT_DISABLED;
            dec->ignoreExif = AVIF_TRUE;
            dec->ignoreXMP = AVIF_TRUE;
            if (avifDecoderSetIOMemory(dec.get(), d, n) != AVIF_RESULT_OK || avifDecoderParse(dec.get()) != AVIF_RESULT_OK)
                return {};
            const avifImage* im = dec->image;
            if (im->icc.size) return Bytes(im->icc.data, im->icc.data + im->icc.size);
            return cicpProfile(im->colorPrimaries, im->transferCharacteristics);
        }
        default: return {};
    }
}

}  // namespace codecs
