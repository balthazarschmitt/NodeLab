#include "nodes/io/IONodes.h"

#include <stdexcept>

#include "io/ImageCache.h"

void ImageInputNode::evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) {
    std::string path = paramS(0);
    if (path.empty() || !ctx.cache) return;  // empty output: nothing loaded yet
    std::string err;
    ImagePtr img = ctx.cache->get(path, ctx.proxy, &err);
    // Name the file: stb's "can't fopen" alone doesn't say which of several inputs failed, and
    // relative paths resolve against the project folder, which is easy to get wrong.
    if (!img) throw std::runtime_error("Image Input: " + err + " (" + path + ")");
    out[0] = Value(img);
}

void registerIONodes(NodeRegistry& r) {
    r.add<ImageInputNode>();
    r.add<OutputNode>();
    r.add<NumberNode>();
}
