#include "ui/ImageView.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "core/Parallel.h"
#include "gpu/Device.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {
// Device textures the UI stopped showing, with the frame they were last drawn in. A frame's
// draws are queued, not done, when it is swapped, and the device may overwrite a pooled
// texture straight away.
std::vector<std::pair<std::shared_ptr<gpu::Texture>, uint64_t>> g_retired;
uint64_t g_frame = 0;
}  // namespace

GLTexture::~GLTexture() { reset(); }

uint32_t GLTexture::id() const { return device_ ? device_->id() : id_; }

void GLTexture::releaseDevice() {
    if (device_) g_retired.emplace_back(std::move(device_), g_frame);
    device_.reset();
}

void GLTexture::endFrame() {
    ++g_frame;
    // Drivers queue at most a few frames.
    std::erase_if(g_retired, [](const auto& r) { return r.second + 4 < g_frame; });
}

void GLTexture::reset() {
    if (id_) {
        GLuint t = id_;
        glDeleteTextures(1, &t);
    }
    id_ = 0;
    releaseDevice();
    w_ = h_ = 0;
}

void GLTexture::showDevice(std::shared_ptr<gpu::Texture> t, int w, int h) {
    releaseDevice();
    device_ = std::move(t);
    w_ = w;
    h_ = h;
}

std::vector<unsigned char> displayBytes(const Image& img, bool clipping) {
    std::vector<unsigned char> bytes(img.px.size());
    parallelFor(img.h, [&](int y) {
        const size_t end = size_t(y + 1) * img.w * 4;
        for (size_t i = size_t(y) * img.w * 4; i < end; i += 4) {
            for (int k = 0; k < 3; ++k)
                bytes[i + k] = static_cast<unsigned char>(std::lround(std::clamp(img.px[i + k], 0.0f, 1.0f) * 255.0f));
            // Straight alpha: the viewer draws the image over a checkerboard (drawImageView).
            const float a = img.px[i + 3];
            bytes[i + 3] = static_cast<unsigned char>(std::lround((a >= 0.0f ? std::min(a, 1.0f) : 0.0f) * 255.0f));
            if (clipping) {
                const unsigned char mx = std::max({bytes[i], bytes[i + 1], bytes[i + 2]});
                if (mx == 255) bytes[i] = 255, bytes[i + 1] = 0, bytes[i + 2] = 0;
                else if (mx == 0) bytes[i] = 0, bytes[i + 1] = 90, bytes[i + 2] = 255;
            }
        }
    });
    return bytes;
}

std::vector<unsigned char> tintBytes(const Image& img, float r, float g, float b, float opacity) {
    std::vector<unsigned char> bytes(img.px.size());
    const auto c8 = [](float v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (size_t i = 0; i < bytes.size(); i += 4) {
        bytes[i] = c8(r), bytes[i + 1] = c8(g), bytes[i + 2] = c8(b);
        bytes[i + 3] = c8(img.px[i] * opacity);
    }
    return bytes;
}

void GLTexture::upload(const Image& img, bool clipping) {
    if (img.empty()) {
        reset();
        return;
    }
    uploadBytes(displayBytes(img, clipping), img.w, img.h);
}

void GLTexture::uploadTint(const Image& img, float r, float g, float b, float opacity) {
    if (img.empty()) {
        reset();
        return;
    }
    uploadBytes(tintBytes(img, r, g, b, opacity), img.w, img.h);
}

void GLTexture::uploadBytes(const std::vector<unsigned char>& bytes, int w, int h) {
    releaseDevice();
    if (!id_) {
        GLuint t = 0;
        glGenTextures(1, &t);
        id_ = t;
    }
    glBindTexture(GL_TEXTURE_2D, id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
    w_ = w;
    h_ = h;
}

void Histogram::compute(const Image& img) {
    r.fill(0), g.fill(0), b.fill(0), l.fill(0);
    valid = !img.empty();
    clipHigh = clipLow = false;
    if (!valid) return;
    // Every pixel of the preview is cheap enough, but skip rows on huge images.
    const int step = std::max(1, int(img.pixelCount() / 2000000));
    auto bin = [](float v) { return std::clamp(int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)), 0, 255); };
    for (int y = 0; y < img.h; y += step)
        for (int x = 0; x < img.w; ++x) {
            const float* p = img.pixel(size_t(y) * img.w + x);
            const int br = bin(p[0]), bg = bin(p[1]), bb = bin(p[2]);
            r[br] += 1, g[bg] += 1, b[bb] += 1;
            l[bin(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2])] += 1;
            if (br == 255 || bg == 255 || bb == 255) clipHigh = true;
            if (br == 0 && bg == 0 && bb == 0) clipLow = true;
        }
    scalePeak();
}

void Histogram::setCounts(const uint32_t* counts) {
    for (int i = 0; i < 256; ++i)
        r[i] = float(counts[i]), g[i] = float(counts[256 + i]), b[i] = float(counts[512 + i]), l[i] = float(counts[768 + i]);
    clipHigh = counts[1024] != 0, clipLow = counts[1025] != 0;
    valid = true;
    scalePeak();
}

void Histogram::scalePeak() {
    // Scale to the tallest inner bin: a spike at pure black or white would flatten the rest.
    peak = 1.0f;
    for (int i = 1; i < 255; ++i) peak = std::max({peak, r[i], g[i], b[i], l[i]});
}

bool drawHistogram(ImDrawList* dl, const ImVec2& pos, const ImVec2& size, const Histogram& h, bool clipOn) {
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, end, IM_COL32(18, 18, 22, 215), 4);
    dl->AddRect(pos, end, IM_COL32(80, 80, 90, 200), 4);
    if (!h.valid) return false;
    const float x0 = pos.x + 4, w = size.x - 8, base = end.y - 4, hgt = size.y - 18;
    // Square-root scale keeps small populations visible next to big ones.
    const float norm = 1.0f / std::sqrt(h.peak);
    auto plot = [&](const std::array<float, 256>& bins, ImU32 fill, ImU32 line) {
        ImVec2 prev;
        for (int i = 0; i < 256; ++i) {
            const ImVec2 p(x0 + w * i / 255.0f, base - std::min(std::sqrt(bins[i]) * norm, 1.0f) * hgt);
            if (i > 0) {
                dl->AddQuadFilled(ImVec2(prev.x, base), prev, p, ImVec2(p.x, base), fill);
                dl->AddLine(prev, p, line, 1.0f);
            }
            prev = p;
        }
    };
    plot(h.l, IM_COL32(200, 200, 200, 45), IM_COL32(220, 220, 220, 140));
    plot(h.r, IM_COL32(255, 60, 60, 55), IM_COL32(255, 90, 90, 200));
    plot(h.g, IM_COL32(60, 255, 60, 55), IM_COL32(90, 230, 90, 200));
    plot(h.b, IM_COL32(70, 110, 255, 55), IM_COL32(110, 140, 255, 220));

    // Clipping triangles: lit when pixels are clipped; clicking either toggles the warning.
    bool clicked = false;
    auto tri = [&](bool right, bool lit, ImU32 litCol) {
        const float s = 10.0f, y = pos.y + 3;
        const float x = right ? end.x - 3 - s : pos.x + 3;
        const ImVec2 a(x, y), b2(x + s, y), c(right ? x + s : x, y + s);
        dl->AddTriangleFilled(a, b2, c, lit ? litCol : IM_COL32(70, 70, 78, 255));
        if (clipOn) dl->AddTriangle(a, b2, c, IM_COL32(255, 255, 255, 230), 1.5f);
        const ImVec2 mp = ImGui::GetIO().MousePos;
        if (mp.x >= x - 3 && mp.x <= x + s + 3 && mp.y >= y - 3 && mp.y <= y + s + 3) {
            ImGui::SetTooltip("%s", right ? "Highlight clipping: click (or J) to show it on the image in red"
                                          : "Shadow clipping: click (or J) to show it on the image in blue");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = true;
        }
    };
    tri(false, h.clipLow, IM_COL32(70, 140, 255, 255));
    tri(true, h.clipHigh, IM_COL32(255, 70, 70, 255));
    return clicked;
}

void averageColor(const Image& img, int x0, int y0, int x1, int y1, float rgb[3]) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::clamp(x0, 0, img.w - 1);
    x1 = std::clamp(x1, 0, img.w - 1);
    y0 = std::clamp(y0, 0, img.h - 1);
    y1 = std::clamp(y1, 0, img.h - 1);
    double sum[3] = {0, 0, 0};
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float* p = img.pixel(size_t(y) * img.w + x);
            for (int k = 0; k < 3; ++k) sum[k] += p[k];
        }
    const double n = double(x1 - x0 + 1) * (y1 - y0 + 1);
    for (int k = 0; k < 3; ++k) rgb[k] = float(sum[k] / n);
}

namespace {
// One eyedropper drag at a time, across all image views.
ImGuiID gPickDragId = 0;
int gPickX0 = 0, gPickY0 = 0;

void drawPicker(PickRequest& pick, ImGuiID id, ImVec2 imgMin, float scale, const GLTexture& tex, ImDrawList* dl) {
    const Image& img = *pick.image;
    const ImGuiIO& io = ImGui::GetIO();
    // Screen <-> image pixels. The image may differ from the texture size if a new result is
    // arriving, so map through the texture's size.
    const float sx = float(img.w) / tex.width(), sy = float(img.h) / tex.height();
    auto toPixel = [&](ImVec2 p, int& x, int& y) {
        x = std::clamp(int(std::floor((p.x - imgMin.x) / scale * sx)), 0, img.w - 1);
        y = std::clamp(int(std::floor((p.y - imgMin.y) / scale * sy)), 0, img.h - 1);
    };
    auto toScreen = [&](float x, float y) { return ImVec2(imgMin.x + x / sx * scale, imgMin.y + y / sy * scale); };

    const bool hovered = ImGui::IsItemHovered();
    int mx, my;
    toPixel(io.MousePos, mx, my);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        pick.cancelled = true;
        gPickDragId = 0;
        return;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        gPickDragId = id;
        gPickX0 = mx;
        gPickY0 = my;
    }
    const bool dragging = gPickDragId == id;
    if (!hovered && !dragging) return;

    int x0 = mx, y0 = my, x1 = mx, y1 = my;
    if (dragging) {
        x0 = std::min(gPickX0, mx);
        y0 = std::min(gPickY0, my);
        x1 = std::max(gPickX0, mx);
        y1 = std::max(gPickY0, my);
    }
    float rgb[3];
    averageColor(img, x0, y0, x1, y1, rgb);

    // Outline the sampled pixels: the rectangle being dragged, or the pixel under the cursor
    // (at least a few screen pixels big so it shows when zoomed out).
    ImVec2 a = toScreen(float(x0), float(y0)), b = toScreen(float(x1 + 1), float(y1 + 1));
    if (b.x - a.x < 5) a.x -= 2, b.x += 2;
    if (b.y - a.y < 5) a.y -= 2, b.y += 2;
    dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 200));
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 230));

    // Swatch and values next to the cursor.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f  %.3f  %.3f", rgb[0], rgb[1], rgb[2]);
    const float sw = 26.0f;
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    ImVec2 p(io.MousePos.x + 18, io.MousePos.y + 18);
    const ImVec2 clipMax = dl->GetClipRectMax();
    if (p.x + sw + ts.x + 16 > clipMax.x) p.x = io.MousePos.x - 18 - sw - ts.x - 16;
    if (p.y + sw + 6 > clipMax.y) p.y = io.MousePos.y - 18 - sw - 6;
    dl->AddRectFilled(p, ImVec2(p.x + sw + ts.x + 16, p.y + sw + 6), IM_COL32(20, 20, 24, 230), 4);
    const auto c8 = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    dl->AddRectFilled(ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + 3 + sw, p.y + 3 + sw),
                      IM_COL32(c8(rgb[0]), c8(rgb[1]), c8(rgb[2]), 255), 3);
    dl->AddText(ImVec2(p.x + sw + 10, p.y + 3 + (sw - ts.y) * 0.5f), IM_COL32(230, 230, 235, 255), buf);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        std::copy(rgb, rgb + 3, pick.rgb);
        pick.done = true;
        gPickDragId = 0;
    }
}
}  // namespace

// A checkerboard under the rectangle a..b (clipped to clipMin..clipMax), as Blender's image editor
// shows transparency. Fixed to the screen, 8-pixel squares; one repeating 2x2 texture.
void drawChecker(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 clipMin, ImVec2 clipMax) {
    static GLuint tex = 0;
    if (!tex) {
        const unsigned char dark = 58, light = 88;
        const unsigned char px[16] = {dark, dark, dark, 255, light, light, light, 255,
                                      light, light, light, 255, dark, dark, dark, 255};
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    a = ImVec2(std::max(a.x, clipMin.x), std::max(a.y, clipMin.y));
    b = ImVec2(std::min(b.x, clipMax.x), std::min(b.y, clipMax.y));
    if (a.x >= b.x || a.y >= b.y) return;
    const float period = 16.0f;  // two squares
    dl->AddImage((ImTextureID)(intptr_t)tex, a, b, ImVec2(a.x / period, a.y / period), ImVec2(b.x / period, b.y / period));
}

void drawImageView(const char* id, const GLTexture& tex, ViewState& view, const char* emptyText, PickRequest* pick,
                   ImageOverlay* overlay, const ViewDetail* detail, ViewInfo* info, SplitView* split) {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, 1.0f);
    avail.y = std::max(avail.y, 1.0f);
    const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
    if (info) {
        *info = ViewInfo{};
        info->panelW = avail.x * fbScale.x;
        info->panelH = avail.y * fbScale.y;
    }

    ImGui::InvisibleButton(id, avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(origin.x + avail.x, origin.y + avail.y);
    dl->AddRectFilled(origin, end, theme::col(theme::ImageBackground));

    if (!tex.valid()) {
        ImVec2 ts = ImGui::CalcTextSize(emptyText);
        dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + (avail.y - ts.y) * 0.5f),
                    IM_COL32(140, 140, 150, 255), emptyText);
        return;
    }

    const float fit = std::min(avail.x / tex.width(), avail.y / tex.height());
    const ImVec2 center(origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f);

    ImGuiIO& io = ImGui::GetIO();
    const bool picking = pick && pick->image && !pick->image->empty();
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0.0f && !(io.KeyCtrl && overlay && overlay->wantsCtrlWheel())) {
            // Zoom around the cursor: keep the image point under the mouse fixed.
            float scaleOld = fit * view.zoom;
            float newZoom = std::clamp(view.zoom * std::pow(1.15f, io.MouseWheel), 0.1f, 64.0f);
            float scaleNew = fit * newZoom;
            float mx = io.MousePos.x - center.x, my = io.MousePos.y - center.y;
            float ix = (mx - view.panX * view.zoom) / scaleOld;  // image coords rel. to center
            float iy = (my - view.panY * view.zoom) / scaleOld;
            view.zoom = newZoom;
            view.panX = (mx - ix * scaleNew) / view.zoom;
            view.panY = (my - iy * scaleNew) / view.zoom;
        }
    }
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();

    const float scale = fit * view.zoom;
    const float hw = tex.width() * scale * 0.5f, hh = tex.height() * scale * 0.5f;
    const float cx = center.x + view.panX * view.zoom, cy = center.y + view.panY * view.zoom;
    dl->PushClipRect(origin, end, true);
    drawChecker(dl, ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), origin, end);
    dl->AddImage((ImTextureID)(intptr_t)tex.id(), ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh));
    if (detail && detail->tex && detail->tex->valid()) {
        // Pixel edges of the detail land on the image's, so it lines up exactly with the preview.
        const float x0 = cx - hw, y0 = cy - hh;
        // A fresh checkerboard under it, so transparent pixels don't show the preview through.
        drawChecker(dl, ImVec2(x0 + detail->u0 * 2 * hw, y0 + detail->v0 * 2 * hh),
                    ImVec2(x0 + detail->u1 * 2 * hw, y0 + detail->v1 * 2 * hh), origin, end);
        dl->AddImage((ImTextureID)(intptr_t)detail->tex->id(), ImVec2(x0 + detail->u0 * 2 * hw, y0 + detail->v0 * 2 * hh),
                     ImVec2(x0 + detail->u1 * 2 * hw, y0 + detail->v1 * 2 * hh));
    }
    if (info) {
        info->imageW = 2 * hw * fbScale.x;
        info->u0 = std::clamp((origin.x - (cx - hw)) / (2 * hw), 0.0f, 1.0f);
        info->u1 = std::clamp((end.x - (cx - hw)) / (2 * hw), 0.0f, 1.0f);
        info->v0 = std::clamp((origin.y - (cy - hh)) / (2 * hh), 0.0f, 1.0f);
        info->v1 = std::clamp((end.y - (cy - hh)) / (2 * hh), 0.0f, 1.0f);
    }
    bool captured = false;
    if (split && split->before && split->before->valid() && split->pos) {
        // The before image is fitted on its own (a crop can change the shape) around the same
        // centre, so zoom and pan move both halves together.
        const GLTexture& bt = *split->before;
        const float bscale = std::min(avail.x / bt.width(), avail.y / bt.height()) * view.zoom;
        const float bhw = bt.width() * bscale * 0.5f, bhh = bt.height() * bscale * 0.5f;
        const ImVec2 b0(cx - bhw, cy - bhh), b1(cx + bhw, cy + bhh);
        float& pos = *split->pos;
        pos = std::clamp(pos, 0.0f, 1.0f);
        const float divX = split->full ? end.x : origin.x + avail.x * pos;
        if (divX > origin.x) {
            const ImVec2 clipMax(divX, end.y);
            dl->PushClipRect(origin, clipMax, true);
            dl->AddRectFilled(origin, clipMax, theme::col(theme::ImageBackground));
            drawChecker(dl, b0, b1, origin, clipMax);
            dl->AddImage((ImTextureID)(intptr_t)bt.id(), b0, b1);
            if (const ViewDetail* d = split->beforeDetail; d && d->tex && d->tex->valid()) {
                const ImVec2 d0(b0.x + d->u0 * 2 * bhw, b0.y + d->v0 * 2 * bhh), d1(b0.x + d->u1 * 2 * bhw, b0.y + d->v1 * 2 * bhh);
                drawChecker(dl, d0, d1, origin, clipMax);
                dl->AddImage((ImTextureID)(intptr_t)d->tex->id(), d0, d1);
            }
            dl->PopClipRect();
        }
        if (split->beforeInfo) {
            ViewInfo& bi = *split->beforeInfo;
            bi.panelW = avail.x * fbScale.x;
            bi.panelH = avail.y * fbScale.y;
            bi.imageW = 2 * bhw * fbScale.x;
            bi.u0 = std::clamp((origin.x - b0.x) / (2 * bhw), 0.0f, 1.0f);
            bi.u1 = std::clamp((std::min(divX, end.x) - b0.x) / (2 * bhw), 0.0f, 1.0f);
            bi.v0 = std::clamp((origin.y - b0.y) / (2 * bhh), 0.0f, 1.0f);
            bi.v1 = std::clamp((end.y - b0.y) / (2 * bhh), 0.0f, 1.0f);
        }
        const auto label = [&](const char* text, float x, bool right) {
            const ImVec2 ts = ImGui::CalcTextSize(text);
            const ImVec2 p(right ? x - ts.x - 14 : x + 8, end.y - ts.y - 14);
            dl->AddRectFilled(ImVec2(p.x - 6, p.y - 3), ImVec2(p.x + ts.x + 6, p.y + ts.y + 3), IM_COL32(20, 20, 24, 200), 4);
            dl->AddText(p, IM_COL32(230, 230, 235, 255), text);
        };
        if (split->full) {
            label("Before", origin.x, false);
        } else {
            // The divider: a line with a round grip, dragged anywhere along it.
            // Only a fresh press grabs it, so a mask handle dragged across the line keeps going.
            const bool near = hovered && !picking && std::fabs(io.MousePos.x - divX) <= 6.0f &&
                              (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left));
            bool& drag = split->dragging ? *split->dragging : captured;
            if (near && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drag = true;
            if (drag) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) pos = std::clamp((io.MousePos.x - origin.x) / avail.x, 0.0f, 1.0f);
                else drag = false;
            }
            if (near || drag) {
                captured = true;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            const float x = origin.x + avail.x * pos, gy = (origin.y + end.y) * 0.5f;
            const ImU32 line = (near || drag) ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 235, 240, 220);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, end.y), IM_COL32(0, 0, 0, 120), 3.0f);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, end.y), line, 1.0f);
            dl->AddCircleFilled(ImVec2(x, gy), 9.0f, IM_COL32(30, 30, 34, 230));
            dl->AddCircle(ImVec2(x, gy), 9.0f, line, 0, 1.5f);
            dl->AddTriangleFilled(ImVec2(x - 6, gy), ImVec2(x - 2, gy - 4), ImVec2(x - 2, gy + 4), line);
            dl->AddTriangleFilled(ImVec2(x + 6, gy), ImVec2(x + 2, gy + 4), ImVec2(x + 2, gy - 4), line);
            if (x - origin.x > 70) label("Before", x, true);
            if (end.x - x > 70) label("After", x, false);
        }
    }
    if (picking) drawPicker(*pick, ImGui::GetItemID(), ImVec2(cx - hw, cy - hh), scale, tex, dl);
    else if (overlay) captured |= overlay->update(dl, ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), hovered && !captured, active && !captured);
    dl->PopClipRect();

    // Pan after the overlay had its say, so dragging a handle doesn't also move the image.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !picking && !captured) view.reset();
    if (active && ((ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && !picking && !captured) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        view.panX += io.MouseDelta.x / view.zoom;
        view.panY += io.MouseDelta.y / view.zoom;
    }
}
