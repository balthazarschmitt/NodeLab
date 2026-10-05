#pragma once
// AI masks, like Lightroom's Select Subject and Select Sky: a model (ml/Models.h) finds the subject
// or the sky in a downscaled copy of the image, and a guided filter fits the mask's edges to the
// full-resolution image. Without the model (it is downloaded from the Inspector) the mask is empty.
#include <atomic>
#include <mutex>
#include "nodes/NodeUtil.h"

class AutoMaskNode : public Node {
public:
    enum { RefineEdges, Invert };

    // The model and the input it was trained on.
    struct Model {
        const char* id;          // ml::ModelSpec id
        int width, height;       // input size (the model's fixed one; tests check it)
        float mean[3], std[3];   // input normalisation, of 0..1 sRGB values
        bool sigmoid;            // the output is logits
    };
    virtual const Model& model() const = 0;

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;
    // Regions reuse the preview's low-resolution mask (EvalContext::previewStats) and read only
    // the edge refinement's reach around them.
    int roiPadding(const EvalContext& ctx) const override;
    // The result changes when the model is installed or removed, or moves between GPU and CPU.
    std::string signatureExtra() const override;

    // The model's mask for an image (probabilities, pw x ph), cached across evaluations. Failed
    // with err set when the model can't run. With `background`, a run that isn't cached starts
    // in the background and the result is Pending, with the mask of a similar picture if there
    // is one (or none); otherwise it waits for the run. Exposed for tests.
    enum class Inferred { Ready, Pending, Failed };
    Inferred infer(const Image& img, bool linear, bool background, std::vector<float>& probs, int& pw, int& ph,
                   std::string& err, const std::atomic<bool>* cancel) const;
    // Edge refinement radius in working pixels, for a full image fullW wide and a pw-wide mask.
    static float refineSigma(int fullW, int fullH, int pw, int ph);

    // Why the model's last picture has no mask (empty when it worked), for the Inspector. Global
    // per model, as evaluations run on copies of the graph.
    std::string lastError() const;

    // The background run, for progress displays (see infer).
    struct Progress {
        bool running = false;
        std::string model;     // its id
        bool loading = false;  // loading the model rather than running it
        double seconds = 0;    // since it started
        double expected = 0;   // what this model's runs took before (or a first guess)
        int queued = 0;        // other runs waiting
    };
    static Progress progress();
    // Bumped when a background run ends: nodes fold it into their cache key, and the app
    // re-evaluates when it changes.
    static int resultGeneration();
    // Waits for the background runs (tests).
    static void waitForRuns();
};

class SelectSubjectNode : public AutoMaskNode {
public:
    NODELAB_NODE({"matte.select_subject", "Select Subject", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Refine Edges", true), ParamDesc::Bool("Invert", false)}})
    const Model& model() const override;
};

class SelectSkyNode : public AutoMaskNode {
public:
    NODELAB_NODE({"matte.select_sky", "Select Sky", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Refine Edges", true), ParamDesc::Bool("Invert", false)}})
    const Model& model() const override;
};
