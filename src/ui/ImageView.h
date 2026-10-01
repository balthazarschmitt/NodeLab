#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "core/Image.h"

struct ImDrawList;
struct ImVec2;

// What GLTexture::upload/uploadTint send to the GPU: 8-bit RGBA.
std::vector<unsigned char> displayBytes(const Image& img, bool clipping);
std::vector<unsigned char> tintBytes(const Image& img, float r, float g, float b, float opacity);

// An OpenGL texture mirroring an Image (converted to 8-bit for display).
class GLTexture {
public:
    GLTexture() = default;
    ~GLTexture();
    GLTexture(const GLTexture&) = delete;
    GLTexture& operator=(const GLTexture&) = delete;

    // clipping: paint pixels with a clipped channel red (highlights) and pure black blue
    // (shadows), like Lightroom's clipping warnings.
    void upload(const Image& img, bool clipping = false);
    // Mask display: a flat colour whose opacity follows the image's red channel.
    void uploadTint(const Image& img, float r, float g, float b, float opacity);
    void reset();
    // Bytes from displayBytes/tintBytes (made off the UI thread by DisplayWorker).
    void uploadBytes(const std::vector<unsigned char>& bytes, int w, int h);
    bool valid() const { return id_ != 0 && w_ > 0; }
    uint32_t id() const { return id_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    uint32_t id_ = 0;
    int w_ = 0, h_ = 0;
};

// Zoom/pan shared by the left and right panes so they stay in sync.
// zoom is relative to "fit to pane"; pan is in fitted-image pixels.
struct ViewState {
    float zoom = 1.0f;
    float panX = 0.0f, panY = 0.0f;
    void reset() { *this = ViewState{}; }
};

// Eyedropper mode for drawImageView: left click picks a pixel, left drag averages a rectangle
// (middle drag still pans), right-click cancels. A swatch next to the cursor shows the colour.
struct PickRequest {
    const Image* image = nullptr;  // the image the texture shows (same size), sampled for colours
    bool done = false;             // out: a pick finished; rgb holds it
    bool cancelled = false;        // out: right-click
    float rgb[3] = {0, 0, 0};
};

// On-image controls drawn over a view (crop frame, mask handles, brush). update() runs after the
// image is drawn, with the image's screen rectangle; it returns true while it owns the left mouse
// (hovering a handle, dragging, painting) so the view doesn't pan.
struct ImageOverlay {
    virtual ~ImageOverlay() = default;
    virtual bool update(ImDrawList* dl, const ImVec2& imgMin, const ImVec2& imgMax, bool hovered, bool active) = 0;
};

// Per-channel histogram (256 bins) of an image, for the viewer's histogram overlay.
struct Histogram {
    std::array<float, 256> r{}, g{}, b{}, l{};
    float peak = 1.0f;  // tallest bin (ignoring the end bins) for scaling
    bool clipHigh = false, clipLow = false;
    bool valid = false;
    void compute(const Image& img);
};

// Average RGB of the pixels in the inclusive rectangle (clamped to the image).
void averageColor(const Image& img, int x0, int y0, int x1, int y1, float rgb[3]);

// A sharper texture of part of the image (a zoomed-in view's detail), drawn over it.
struct ViewDetail {
    const GLTexture* tex = nullptr;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // where it sits, in 0..1 of the image
};

// What a view showed, in framebuffer pixels (for the proxy size and detail requests).
struct ViewInfo {
    float panelW = 0, panelH = 0;          // the view's area
    float imageW = 0;                      // width the whole image is drawn at
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // visible part of the image, in 0..1
};

// Draws the texture fitted to the current region with zoom/pan interaction.
// Wheel zooms around the cursor, left/middle drag pans, double-click resets.
// With pick set (and pick->image non-null), left mouse picks colours instead of panning.
void drawImageView(const char* id, const GLTexture& tex, ViewState& view, const char* emptyText,
                   PickRequest* pick = nullptr, ImageOverlay* overlay = nullptr, const ViewDetail* detail = nullptr,
                   ViewInfo* info = nullptr);

// Histogram box (RGB + luminance) drawn at `pos`, with Lightroom's clipping triangles in the top
// corners. Returns true when a triangle was clicked (toggles the clipping warning).
bool drawHistogram(ImDrawList* dl, const ImVec2& pos, const ImVec2& size, const Histogram& h, bool clipOn);
