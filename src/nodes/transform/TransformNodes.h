#pragma once
#include <array>
#include <mutex>
#include <string>
#include <vector>

#include "graph/Node.h"
#include "io/LensProfiles.h"
#include "nodes/NodeUtil.h"

// Crop node helpers shared with the viewer's crop overlay.
// Pan and Zoom: the image moved and scaled within its own frame, transparent where it doesn't
// reach; dragged in the viewer (ViewerOverlay).
namespace panzoom {
constexpr const char* kType = "xform.pan_zoom";
enum Param { Zoom = 0, X, Y, Interpolation };
}  // namespace panzoom

namespace crop {

constexpr const char* kType = "xform.crop";
// Param indices of the Crop node.
enum { Left = 0, Right, Top, Bottom, ResizeImage, Angle, Aspect, Constrain };

// Width / height of the Aspect option, or 0 for Free. `imageW/H` is used by "Original".
float aspectRatio(int option, int imageW, int imageH);

struct Rect {
    float l = 0, r = 1, t = 0, b = 1;  // fractions of the image
};

// The crop rectangle after the aspect ratio is applied (shrunk around its centre to fit).
Rect effectiveRect(const Node& n, int imageW, int imageH);

}  // namespace crop

// Lightroom's Transform panel: Upright (Guided: lines drawn on the image become vertical or
// horizontal), Vertical / Horizontal perspective, Rotate, Aspect, Scale and offsets. The
// perspective is a camera rotation (a homography), as when the photo had been taken level.
namespace perspective {

constexpr const char* kType = "xform.perspective";
enum { Upright = 0, Vertical, Horizontal, Rotate, Aspect, Scale, OffsetX, OffsetY, Constrain };
// Values are saved: append new modes. Auto, Level, Vertical and Full find the lines themselves.
enum { UprightOff = 0, UprightGuided, UprightAuto, UprightLevel, UprightVertical, UprightFull };
inline bool uprightDetects(int mode) { return mode >= UprightAuto && mode <= UprightFull; }
constexpr int kMaxGuides = 4;

// A Guided Upright line, image-relative (0..1) in the node's input. Steeper than 45 degrees it
// becomes vertical, otherwise horizontal.
struct Guide {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool vertical(float aspect) const;  // aspect: the image's width / height
};

using Mat = std::array<double, 9>;
// Maps (u, v) through a homography; false when it lands behind the camera.
bool apply(const Mat& m, double u, double v, double& su, double& sv);
Mat inverse(const Mat& m);

// A line segment found in an image, image-relative (0..1).
struct Segment {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
// Straight edges in the image (a small line segment detector on a copy at most 800 pixels
// across), longest regions first. `linear`: the image is scene-linear.
std::vector<Segment> detectLines(const Image& img, bool linear);

// A line for the Upright solver: centred, in units of the half diagonal; its residual is
// scaled by weight.
struct UprightLine {
    double x0, y0, x1, y1;
    bool vertical;
    double weight;
};
// The camera rotation (radians about x, y, z) that makes the lines vertical or horizontal,
// turning only the axes marked free.
void solveUpright(const std::vector<UprightLine>& lines, const bool free[3], double angles[3]);
// Auto, Level, Vertical or Full Upright's rotation from detected segments, for a w x h image.
void autoUprightAngles(const std::vector<Segment>& segments, int w, int h, int mode, double angles[3]);

}  // namespace perspective

class PerspectiveNode : public Node {
public:
    REFRACTORY_NODE({perspective::kType, "Perspective", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Enum("Upright", perspective::UprightGuided, {"Off", "Guided", "Auto", "Level", "Vertical", "Full"}),
                   ParamDesc::Float("Vertical", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Horizontal", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Rotate", 0.0f, -10.0f, 10.0f), ParamDesc::Float("Aspect", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Scale", 100.0f, 50.0f, 150.0f), ParamDesc::Float("X Offset", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Y Offset", 0.0f, -100.0f, 100.0f), ParamDesc::Bool("Constrain Crop", false)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override;
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;

    void saveExtra(nlohmann::json& j) const override;
    void loadExtra(const nlohmann::json& j) override;
    std::string signatureExtra() const override;

    std::vector<perspective::Guide> guides;  // used while Upright is Guided

    // Output to input, image-relative (0..1) coordinates, for a w x h image (only its shape
    // matters). `upright` overrides the Upright rotation; otherwise it's the guides' or the one
    // the last evaluation detected.
    perspective::Mat matrix(int w, int h, const double* upright = nullptr) const;
    // The camera rotation Guided Upright solves for (radians about x, y, z); zeros without guides.
    void guidedAngles(int w, int h, double angles[3]) const;
    // The detecting modes' rotation found by the last evaluation (zeros before one).
    void detectedAngles(double angles[3]) const;
    // The Upright rotation for this evaluation: the guides', or lines found in `src`. Regions reuse
    // the preview's (ctx.previewStats), so a zoomed-in view matches the whole image.
    void uprightAngles(EvalContext& ctx, const Image* src, int w, int h, double angles[3]);

private:
    mutable std::mutex detectedMutex_;
    double detected_[3] = {0, 0, 0};
};

// Lightroom's "Enable Profile Corrections": the distortion, chromatic aberration and vignetting a
// lens profile (io/LensProfiles.h) measured, undone at the photo's focal length and aperture.
// The resolved profile is part of the node, so the project renders without the database.
class LensProfileNode : public Node {
public:
    enum { Distortion = 0, ChromaticAberration, Vignetting, Constrain };
    REFRACTORY_NODE({"xform.lens_profile", "Lens Profile", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Distortion", 100.0f, 0.0f, 200.0f), ParamDesc::Bool("Chromatic Aberration", true),
                   ParamDesc::Float("Vignetting", 100.0f, 0.0f, 200.0f), ParamDesc::Bool("Constrain Crop", false)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override;
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;

    void saveExtra(nlohmann::json& j) const override;
    void loadExtra(const nlohmann::json& j) override;
    std::string signatureExtra() const override;

    lensdb::Profile profile;  // none until detected or chosen
    bool detectTried = false;  // the Inspector looked the photo's lens up once (not saved)

    // Where the output pixel at radius r (the half diagonal is 1, before the Constrain Crop zoom)
    // reads the photo's red, green and blue channels, as multiples of its offset from the centre;
    // and the vignetting gain there. Exposed for tests.
    struct Sample {
        float m[3];
        float gain;
    };
    Sample sampleAt(float r) const;
    // The zoom Constrain Crop applies (1 without it) for a w x h image.
    float zoom(int w, int h) const;
};
