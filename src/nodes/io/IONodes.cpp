#include "nodes/io/IONodes.h"

#include <stdexcept>

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
    out[0] = Value(img);
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
