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

// Draws the texture fitted to the current region with zoom/pan interaction.
// Wheel zooms around the cursor, left/middle drag pans, double-click resets.
void drawImageView(const char* id, const GLTexture& tex, ViewState& view, const char* emptyText);
