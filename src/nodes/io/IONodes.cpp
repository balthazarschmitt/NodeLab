#include "nodes/io/IONodes.h"

#include <cmath>
#include <stdexcept>

#include "core/Parallel.h"

#include "io/ImageCache.h"

void ImageInputNode::evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) {
    std::string path = paramS(0);
    if (path.empty() || !ctx.cache) return;  // empty output: nothing loaded yet
    std::string err;
    // A region (zoomed-in viewer) reads the full-resolution level at the region's scale.
    ImagePtr img = ctx.roi ? ctx.cache->level(path, ctx.scale, &err, decode(ctx.linear()))
                           : ctx.cache->get(path, ctx.proxy, &err, decode(ctx.linear()), ctx.proxyEdge);
    // Name the file: stb's "can't fopen" alone doesn't say which of several inputs failed, and
    // relative paths resolve against the project folder, which is easy to get wrong.
    if (!img) throw std::runtime_error("Image Input: " + err + " (" + path + ")");
    if (ctx.roi) {
        const PixelRect& r = ctx.roi->rect;
        if (img->w != ctx.roi->canvasW || img->h != ctx.roi->canvasH)
            throw std::runtime_error("Image Input: region does not match the image");
        img = cropImage(*img, r.x, r.y, r.w, r.h);
    }
    out[0] = Value(applyExposure(std::move(img)));
}

bool ImageInputNode::chooseFile(const std::string& pathU8) {
    const bool wasRaw = raw::isRawPath(paramS(0));
    params[0] = pathU8;
    if (wasRaw || !raw::isRawPath(pathU8)) return false;
    params[3] = kRawBaselineEV;
    params[4] = true;
    return true;
}

float ImageInputNode::exposureGain() const {
    const std::string& path = paramS(0);
    if (path.empty() || !raw::isRawPath(path)) return 1.0f;
    float ev = paramF(3);
    if (paramB(4)) ev -= raw::exposureBias(path);
    return std::exp2(ev);
}

ImagePtr ImageInputNode::applyExposure(ImagePtr img) const {
    const float gain = exposureGain();
    if (!img || gain == 1.0f) return img;
    auto out = std::make_shared<Image>(*img);
    parallelFor(out->h, [&](int y) {
        float* p = out->pixel(size_t(y) * out->w);
        for (int x = 0; x < out->w; ++x, p += 4)
            for (int c = 0; c < 3; ++c) p[c] *= gain;
    });
    return out;
}

bool ImageInputNode::roiSourceSize(const EvalContext& ctx, int& w, int& h) const {
    int fw = 0, fh = 0;
    if (!ctx.cache || !ctx.cache->fullSize(paramS(0), fw, fh)) return false;
    ImageCache::levelSize(fw, fh, ctx.scale, w, h);
    return true;
}

void registerIONodes(NodeRegistry& r) {
    r.add<ImageInputNode>();
    r.add<OutputNode>();
    r.add<NumberNode>();
}
