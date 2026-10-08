#pragma once
// AI masks, like Lightroom's Select Subject and Select Sky: a model (ml/Models.h) finds the subject
// or the sky in a downscaled copy of the image, and a guided filter fits the mask's edges to the
// full-resolution image. Without the model (it is downloaded from the Inspector) the mask is empty.
#include <atomic>
#include <mutex>
#include "nodes/NodeUtil.h"

class AutoMaskNode : public Node {
public:
    enum { RefineEdges, Invert, ModelChoice };

    // The model and the input it was trained on.
    struct Model {
        const char* id;          // ml::ModelSpec id
        int width, height;       // input size (the model's fixed one; tests check it)
        float mean[3], std[3];   // input normalisation, of 0..1 sRGB values
        bool sigmoid;            // the output is logits
        bool stretch = false;    // rescale the output to 0..1 (rembg does for U²-Net)
        int classes = 0;         // a segmentation model's class count (its output is logits per
                                 // class); 0 for a model with one mask
    };
    virtual const Model& model() const = 0;
    // For a segmentation model: the classes whose probabilities add up to the mask.
    virtual std::vector<int> selectedClasses() const { return {}; }
    // What the cached masks are kept under: the model, and the classes for a segmentation model
    // (a different choice is another mask, from one more run of the model).
    std::string maskKey() const;
    // Where Feather is in the params; Edge follows it. (They were added after the others, so
    // they sit at the end, where each node's params differ.)
    virtual int featherParam() const = 0;
    // Feather's and Edge's blur radii in working pixels, for a full image fullW x fullH.
    float featherSigma(int fullW, int fullH) const;
    float edgeSigma(int fullW, int fullH) const;

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
    // Drops the waiting background runs and stops the running one, when another project or photo
    // opens: a run takes up to a minute and gigabytes, and its picture is no longer on screen.
    static void cancelRuns();
};

class SelectSubjectNode : public AutoMaskNode {
public:
    REFRACTORY_NODE({"matte.select_subject", "Select Subject", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Refine Edges", true), ParamDesc::Bool("Invert", false),
                   ParamDesc::Enum("Model", 0, {"Accurate", "Light"}), ParamDesc::Float("Feather", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Edge", 0.0f, -100.0f, 100.0f)}})
    // Accurate is BiRefNet (a minute, 4 GB on a CPU); Light is U²-Net small (a second, 4.6 MB).
    // Projects from before the choice load as Accurate.
    const Model& model() const override;
    int featherParam() const override { return 3; }
};

// Lightroom's Select People, for portraits: face parts from a face-parsing model trained on
// close-up faces (CelebAMask-HQ), so it works when a face fills much of the frame.
class SelectPeopleNode : public AutoMaskNode {
public:
    enum { FaceSkin = 2, Eyebrows, Eyes, Lips, Mouth, Hair, Neck, Clothes, Accessories };
    REFRACTORY_NODE({"matte.select_people", "Select People", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Refine Edges", true), ParamDesc::Bool("Invert", false),
                   ParamDesc::Bool("Face Skin", true), ParamDesc::Bool("Eyebrows", false), ParamDesc::Bool("Eyes", false),
                   ParamDesc::Bool("Lips", false), ParamDesc::Bool("Mouth", false), ParamDesc::Bool("Hair", false),
                   ParamDesc::Bool("Neck", false), ParamDesc::Bool("Clothes", false), ParamDesc::Bool("Accessories", false),
                   ParamDesc::Float("Feather", 0.0f, 0.0f, 100.0f), ParamDesc::Float("Edge", 0.0f, -100.0f, 100.0f)}})
    const Model& model() const override;
    std::vector<int> selectedClasses() const override;
    int featherParam() const override { return 11; }
};

// Lightroom's Select Landscape: parts of a scene from an ADE20K segmentation model.
class SelectLandscapeNode : public AutoMaskNode {
public:
    enum { Sky = 2, Water, Vegetation, Mountains, NaturalGround, Architecture, ArtificialGround };
    REFRACTORY_NODE({"matte.select_landscape", "Select Landscape", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Refine Edges", true), ParamDesc::Bool("Invert", false), ParamDesc::Bool("Sky", false),
                   ParamDesc::Bool("Water", true), ParamDesc::Bool("Vegetation", false), ParamDesc::Bool("Mountains", false),
                   ParamDesc::Bool("Natural Ground", false), ParamDesc::Bool("Architecture", false),
                   ParamDesc::Bool("Artificial Ground", false), ParamDesc::Float("Feather", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Edge", 0.0f, -100.0f, 100.0f)}})
    const Model& model() const override;
    std::vector<int> selectedClasses() const override;
    int featherParam() const override { return 9; }
};

// Lightroom's Select Objects, by kind rather than by brushing: the same ADE20K model's things.
// Wire a Brush or shape mask to Mask to keep one of several.
class SelectObjectsNode : public AutoMaskNode {
public:
    enum { People = 2, Animals, Vehicles, Furniture, SignsPoles, Lights, Screens, Plants };
    REFRACTORY_NODE({"matte.select_objects", "Select Objects", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Refine Edges", true), ParamDesc::Bool("Invert", false), ParamDesc::Bool("People", true),
                   ParamDesc::Bool("Animals", false), ParamDesc::Bool("Vehicles", false), ParamDesc::Bool("Furniture", false),
                   ParamDesc::Bool("Signs and Poles", false), ParamDesc::Bool("Lights", false), ParamDesc::Bool("Screens", false),
                   ParamDesc::Bool("Potted Plants and Flowers", false), ParamDesc::Float("Feather", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Edge", 0.0f, -100.0f, 100.0f)}})
    const Model& model() const override;
    std::vector<int> selectedClasses() const override;
    int featherParam() const override { return 10; }
};

class SelectSkyNode : public AutoMaskNode {
public:
    REFRACTORY_NODE({"matte.select_sky", "Select Sky", "Matte",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Bool("Refine Edges", true), ParamDesc::Bool("Invert", false),
                   ParamDesc::Float("Feather", 0.0f, 0.0f, 100.0f), ParamDesc::Float("Edge", 0.0f, -100.0f, 100.0f)}})
    const Model& model() const override;
    int featherParam() const override { return 2; }
};
