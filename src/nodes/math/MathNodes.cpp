#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

class MixNode : public Node {
public:
    NODELAB_NODE({"math.mix", "Mix", "Math / Mix",
                  {{"A", PinType::Image}, {"B", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 0.5f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ImagePtr a = toImage(in[0], w, h), b = toImage(in[1], w, h);
        if (!a && !b) return;
        if (!a) { out[0] = Value(b); return; }
        if (!b) { out[0] = Value(a); return; }
        ChannelPtr fac = channelOr(in[2], 0.5f);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ImageSampler sa{a.get(), w, h}, sb{b.get(), w, h};
            ChannelSampler sf{fac.get(), w, h};
            for (int x = 0; x < w; ++x) {
                const float* pa = sa(x, y);
                const float* pb = sb(x, y);
                float f = sf(x, y);
                float* d = img->pixel(size_t(y) * w + x);
                for (int k = 0; k < 4; ++k) d[k] = pa[k] + (pb[k] - pa[k]) * f;
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

}  // namespace

void registerMathNodes(NodeRegistry& r) {
    r.add<MixNode>();
}
