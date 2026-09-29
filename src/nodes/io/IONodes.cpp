#include "nodes/io/IONodes.h"

#include <stdexcept>

#include "io/ImageCache.h"

void ImageInputNode::evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) {
    std::string path = paramS(0);
    if (path.empty() || !ctx.cache) return;  // empty output: nothing loaded yet
    std::string err;
    ImagePtr img = ctx.cache->get(path, ctx.proxy, &err);
    if (!img) throw std::runtime_error("Image Input: " + err);
    out[0] = Value(img);
}

void registerIONodes(NodeRegistry& r) {
    r.add<ImageInputNode>();
    r.add<OutputNode>();
    r.add<NumberNode>();
}
