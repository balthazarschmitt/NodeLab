#pragma once
#include <cstdint>

#include "core/Image.h"

// An OpenGL texture mirroring an Image (converted to 8-bit for display).
class GLTexture {
public:
    GLTexture() = default;
    ~GLTexture();
    GLTexture(const GLTexture&) = delete;
    GLTexture& operator=(const GLTexture&) = delete;

    void upload(const Image& img);
    void reset();
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

// Average RGB of the pixels in the inclusive rectangle (clamped to the image).
void averageColor(const Image& img, int x0, int y0, int x1, int y1, float rgb[3]);

// Draws the texture fitted to the current region with zoom/pan interaction.
// Wheel zooms around the cursor, left/middle drag pans, double-click resets.
// With pick set (and pick->image non-null), left mouse picks colours instead of panning.
void drawImageView(const char* id, const GLTexture& tex, ViewState& view, const char* emptyText,
                   PickRequest* pick = nullptr);
