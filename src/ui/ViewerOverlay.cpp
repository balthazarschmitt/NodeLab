#include "ui/ViewerOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "nodes/filter/SpotRemoval.h"
#include "nodes/matte/MatteNodes.h"
#include "nodes/transform/TransformNodes.h"

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHit = 9.0f;  // handle grab radius, screen pixels

const std::string kLinear = "matte.linear_gradient";
const std::string kRadial = "matte.radial_gradient";
const std::string kBox = "matte.box_mask";
const std::string kEllipse = "matte.ellipse_mask";

float dist(const ImVec2& a, const ImVec2& b) { return std::hypot(a.x - b.x, a.y - b.y); }

void drawHandle(ImDrawList* dl, const ImVec2& p, bool hot, bool square = false) {
    const float r = hot ? 6.0f : 5.0f;
    const ImU32 fill = hot ? IM_COL32(255, 200, 80, 255) : IM_COL32(245, 245, 245, 255);
    if (square) {
        dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), fill);
        dl->AddRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), IM_COL32(0, 0, 0, 220));
    } else {
        dl->AddCircleFilled(p, r, fill);
        dl->AddCircle(p, r, IM_COL32(0, 0, 0, 220), 0, 1.5f);
    }
}

// A line with a dark outline so it reads on light and dark images.
void outlinedLine(ImDrawList* dl, const ImVec2& a, const ImVec2& b, ImU32 col, float t = 1.5f) {
    dl->AddLine(a, b, IM_COL32(0, 0, 0, 150), t + 2.0f);
    dl->AddLine(a, b, col, t);
}

void outlinedPoly(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col, float t = 1.5f) {
    dl->AddPolyline(pts, n, IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, t + 2.0f);
    dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, t);
}

void label(ImDrawList* dl, const ImVec2& p, const char* text) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(p.x - 3, p.y - 2), ImVec2(p.x + ts.x + 3, p.y + ts.y + 2), IM_COL32(20, 20, 24, 220), 3);
    dl->AddText(p, IM_COL32(235, 235, 240, 255), text);
}

// Lines across the rectangle p0..p1: `n` cells along its long side, square cells.
void gridLines(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, int n, ImU32 col) {
    const float w = p1.x - p0.x, h = p1.y - p0.y, cell = std::max(w, h) / n;
    if (cell < 2.0f) return;
    // Centred, so the middle lines meet at the centre of the frame.
    const float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
    for (float x = cx - std::floor((cx - p0.x) / cell) * cell; x < p1.x; x += cell) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), col);
    for (float y = cy - std::floor((cy - p0.y) / cell) * cell; y < p1.y; y += cell) dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), col);
}

// Lightroom's crop guide overlays inside the frame p0..p1. `turn` mirrors the asymmetric ones
// (bit 0 left-right, bit 1 top-bottom).
// `alpha` is the lines' opacity (the spiral's is a little stronger).
void drawCropGuide(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, int guide, int turn, int alpha = 110) {
    const ImU32 col = IM_COL32(255, 255, 255, alpha);
    const float W = p1.x - p0.x, H = p1.y - p0.y;
    const bool fx = turn & 1, fy = turn & 2;
    // Frame-relative (0..1) to screen, mirrored by `turn`.
    const auto at = [&](float u, float v) { return ImVec2(p0.x + (fx ? 1 - u : u) * W, p0.y + (fy ? 1 - v : v) * H); };
    const auto line = [&](float u0, float v0, float u1, float v1) { dl->AddLine(at(u0, v0), at(u1, v1), col); };
    switch (guide) {
    case NodeOverlay::Grid: gridLines(dl, p0, p1, 12, col); break;
    case NodeOverlay::Thirds:
        for (int i = 1; i < 3; ++i) line(i / 3.0f, 0, i / 3.0f, 1), line(0, i / 3.0f, 1, i / 3.0f);
        break;
    case NodeOverlay::Diagonal: {
        // 45-degree lines in from each corner, as far as the frame allows.
        const float d = std::min(W, H);
        dl->AddLine(p0, ImVec2(p0.x + d, p0.y + d), col);
        dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p1.x - d, p0.y + d), col);
        dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p0.x + d, p1.y - d), col);
        dl->AddLine(p1, ImVec2(p1.x - d, p1.y - d), col);
        break;
    }
    case NodeOverlay::Triangle: {
        // A diagonal, and lines from the other two corners meeting it at right angles.
        const ImVec2 a = at(0, 0), b = at(1, 1);
        dl->AddLine(a, b, col);
        const float dx = b.x - a.x, dy = b.y - a.y, len2 = std::max(dx * dx + dy * dy, 1e-6f);
        for (const ImVec2& c : {at(1, 0), at(0, 1)}) {
            const float t = ((c.x - a.x) * dx + (c.y - a.y) * dy) / len2;
            dl->AddLine(c, ImVec2(a.x + dx * t, a.y + dy * t), col);
        }
        break;
    }
    case NodeOverlay::GoldenRatio: {
        constexpr float g = 0.381966f;  // 1 - 1/phi
        for (float f : {g, 1 - g}) line(f, 0, f, 1), line(0, f, 1, f);
        break;
    }
    case NodeOverlay::GoldenSpiral: {
        // Golden rectangles: cut a square off each remaining rectangle in turn (left, top,
        // right, bottom) and draw a quarter arc across it; the arcs join into the spiral.
        constexpr float k = 0.618034f;
        float x = 0, y = 0, w = 1, h = 1;
        ImVec2 pts[17];
        for (int i = 0; i < 10; ++i) {
            float cu, cv, su, sv, eu, ev;  // arc centre, start and end
            switch (i % 4) {
            case 0: {
                const float s = w * k;
                cu = x + s, cv = y + h, su = x, sv = y + h, eu = x + s, ev = y;
                line(x + s, y, x + s, y + h);
                x += s, w -= s;
                break;
            }
            case 1: {
                const float s = h * k;
                cu = x, cv = y + s, su = x, sv = y, eu = x + w, ev = y + s;
                line(x, y + s, x + w, y + s);
                y += s, h -= s;
                break;
            }
            case 2: {
                const float s = w * k;
                cu = x + w - s, cv = y, su = x + w, sv = y, eu = x + w - s, ev = y + h;
                line(x + w - s, y, x + w - s, y + h);
                w -= s;
                break;
            }
            default: {
                const float s = h * k;
                cu = x + w, cv = y + h - s, su = x + w, sv = y + h, eu = x, ev = y + h - s;
                line(x, y + h - s, x + w, y + h - s);
                h -= s;
                break;
            }
            }
            for (int j = 0; j <= 16; ++j) {
                const float t = j / 16.0f * kPi * 0.5f, c = std::cos(t), sn = std::sin(t);
                pts[j] = at(cu + (su - cu) * c + (eu - cu) * sn, cv + (sv - cv) * c + (ev - cv) * sn);
            }
            dl->AddPolyline(pts, 17, IM_COL32(255, 255, 255, std::min(255, alpha * 17 / 11)), ImDrawFlags_None, 1.5f);
        }
        break;
    }
    case NodeOverlay::AspectRatios: {
        // Common print and screen ratios, centred and as large as the frame allows.
        for (float r : {1.0f, 5.0f / 4, 4.0f / 3, 3.0f / 2, 16.0f / 9}) {
            const float ratio = W >= H ? r : 1 / r;
            float w = W, h = W / ratio;
            if (h > H) h = H, w = H * ratio;
            const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            dl->AddRect(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f), col);
        }
        break;
    }
    default: break;
    }
}

// A custom guide (Preferences > Viewer) across p0..p1.
void drawCustomGuide(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, const CustomGuide& g, int alpha) {
    const ImU32 col = IM_COL32(255, 255, 255, alpha);
    const float W = p1.x - p0.x, H = p1.y - p0.y;
    const int cols = std::clamp(g.columns, 1, 64), rows = std::clamp(g.rows, 1, 64);
    // Lines closer than a few pixels only grey the image out.
    if (W / cols >= 3.0f)
        for (int i = 1; i < cols; ++i) dl->AddLine(ImVec2(p0.x + W * i / cols, p0.y), ImVec2(p0.x + W * i / cols, p1.y), col);
    if (H / rows >= 3.0f)
        for (int i = 1; i < rows; ++i) dl->AddLine(ImVec2(p0.x, p0.y + H * i / rows), ImVec2(p1.x, p0.y + H * i / rows), col);
    if (g.diagonals) {
        dl->AddLine(p0, p1, col);
        dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p0.x, p1.y), col);
    }
    if (g.center) {
        const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        const float r = std::min(12.0f, std::min(W, H) * 0.1f);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, 1.5f);
    }
    if (g.safeArea > 0) {
        const float f = std::clamp(g.safeArea, 0.0f, 45.0f) / 100.0f;
        dl->AddRect(ImVec2(p0.x + W * f, p0.y + H * f), ImVec2(p1.x - W * f, p1.y - H * f), col);
    }
}

}  // namespace

const char* NodeOverlay::cropGuideName(int g) {
    static const char* names[kCropGuideCount] = {"Grid", "Thirds", "Diagonal", "Triangle", "Golden Ratio", "Golden Spiral", "Aspect Ratios"};
    return g >= 0 && g < kCropGuideCount ? names[g] : "";
}

bool NodeOverlay::supports(const Node& n) {
    const std::string& t = n.info().type;
    return t == crop::kType || t == perspective::kType || t == panzoom::kType || isMask(n) ||
           dynamic_cast<const SpotRemovalNode*>(&n);
}

bool NodeOverlay::wantsCtrlWheel() const { return node_ && node_->info().type == panzoom::kType; }

bool NodeOverlay::isMask(const Node& n) {
    const std::string& t = n.info().type;
    return t == kLinear || t == kRadial || t == kBox || t == kEllipse || dynamic_cast<const BrushMaskNode*>(&n);
}

void NodeOverlay::setParam(int i, float v) {
    if (!node_ || i >= int(node_->params.size())) return;
    const ParamDesc& d = node_->info().params[i];
    v = std::clamp(v, d.hardMin, d.hardMax);
    if (node_->params[i].is_number() && node_->paramF(i) == v) return;
    node_->params[i] = v;
    changed_ = true;
}

bool NodeOverlay::update(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    if (!node_ || b.x - a.x < 2 || b.y - a.y < 2) return false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = -1;
    if (mask_ && mask_->valid() && isMask(*node_)) dl->AddImage((ImTextureID)(intptr_t)mask_->id(), a, b);
    const std::string& t = node_->info().type;
    if (t == crop::kType) return updateCrop(dl, a, b, hovered, active);
    if (t == perspective::kType) return updatePerspective(dl, a, b, hovered, active);
    if (t == panzoom::kType) return updatePanZoom(dl, a, b, hovered, active);
    if (t == kLinear) return updateLinear(dl, a, b, hovered, active);
    if (t == kRadial || t == kBox || t == kEllipse) return updateShape(dl, a, b, hovered, active);
    if (dynamic_cast<BrushMaskNode*>(node_)) return updateBrush(dl, a, b, hovered, active);
    if (dynamic_cast<SpotRemovalNode*>(node_)) return updateSpots(dl, a, b, hovered, active);
    return false;
}

// ---------------------------------------------------------------- pan and zoom

// Drag anywhere on the image to move it, Ctrl+wheel to zoom about the frame's centre. The moved
// picture's outline is drawn, as it can sit partly outside the frame.
bool NodeOverlay::updatePanZoom(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    const float W = b.x - a.x, H = b.y - a.y;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const float z = std::max(node_->paramF(panzoom::Zoom), 1e-3f);
    const ImVec2 c((a.x + b.x) * 0.5f + node_->paramF(panzoom::X) * W, (a.y + b.y) * 0.5f + node_->paramF(panzoom::Y) * H);
    const ImVec2 p0(c.x - W * z * 0.5f, c.y - H * z * 0.5f), p1(c.x + W * z * 0.5f, c.y + H * z * 0.5f);
    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(0, 0, 0, 120), 0, 0, 3.0f);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 200), 0, 0, 1.0f);
    const bool inside = m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y;
    if (hovered && inside && io.KeyCtrl && io.MouseWheel != 0.0f) {
        setParam(panzoom::Zoom, z * std::pow(1.1f, io.MouseWheel));
        // Zoom about the frame's centre: the offset scales with the picture.
        const float k = node_->paramF(panzoom::Zoom) / z;
        setParam(panzoom::X, node_->paramF(panzoom::X) * k);
        setParam(panzoom::Y, node_->paramF(panzoom::Y) * k);
    }
    if (hovered && inside && drag_ < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    const float mu = (m.x - a.x) / W, mv = (m.y - a.y) / H;
    if (hovered && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag_ = 0;
        grab_[0] = node_->paramF(panzoom::X), grab_[1] = node_->paramF(panzoom::Y);
        grabX_ = mu, grabY_ = mv;
    }
    if (drag_ < 0 || !active) return hovered && inside;
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    setParam(panzoom::X, grab_[0] + mu - grabX_);
    setParam(panzoom::Y, grab_[1] + mv - grabY_);
    return true;
}

// ---------------------------------------------------------------- crop

bool NodeOverlay::updateCrop(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    const float W = b.x - a.x, H = b.y - a.y;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float mu = (m.x - a.x) / W, mv = (m.y - a.y) / H;
    const int iw = std::max(1, int(std::lround(W * 8))), ih = std::max(1, int(std::lround(H * 8)));
    const crop::Rect rc = crop::effectiveRect(*node_, iw, ih);
    const float ar = crop::aspectRatio(node_->paramI(crop::Aspect), iw, ih);
    const ImVec2 p0(a.x + rc.l * W, a.y + rc.t * H), p1(a.x + rc.r * W, a.y + rc.b * H);

    // Dim what will be cut off, then the frame and its guide overlay.
    const ImU32 dim = IM_COL32(0, 0, 0, 150);
    dl->AddRectFilled(a, ImVec2(b.x, p0.y), dim);
    dl->AddRectFilled(ImVec2(a.x, p1.y), b, dim);
    dl->AddRectFilled(ImVec2(a.x, p0.y), ImVec2(p0.x, p1.y), dim);
    dl->AddRectFilled(ImVec2(p1.x, p0.y), ImVec2(b.x, p1.y), dim);
    // A fine grid while straightening, to line up horizons (as Lightroom does).
    if (drag_ == 9) gridLines(dl, p0, p1, 16, IM_COL32(255, 255, 255, 90));
    else drawCropGuide(dl, p0, p1, cropGuide, cropGuideTurn);
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 150), 0, 0, 3.5f);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 235), 0, 0, 1.5f);

    // Handles: 0-3 corners (TL, TR, BR, BL), 4-7 edges (L, R, T, B), 8 move, 9 straighten.
    const ImVec2 hp[8] = {p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y),
                          ImVec2(p0.x, (p0.y + p1.y) * 0.5f), ImVec2(p1.x, (p0.y + p1.y) * 0.5f),
                          ImVec2((p0.x + p1.x) * 0.5f, p0.y), ImVec2((p0.x + p1.x) * 0.5f, p1.y)};
    int hot = drag_;
    if (hot < 0 && hovered) {
        for (int i = 0; i < 8 && hot < 0; ++i)
            if (dist(hp[i], m) < kHit + 2) hot = i;
        if (hot < 0) hot = (m.x > p0.x && m.x < p1.x && m.y > p0.y && m.y < p1.y) ? 8 : 9;
    }
    for (int i = 0; i < 8; ++i) drawHandle(dl, hp[i], hot == i, true);
    if (hot >= 0) {
        static const ImGuiMouseCursor cursors[10] = {
            ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNESW,
            ImGuiMouseCursor_ResizeEW,   ImGuiMouseCursor_ResizeEW,   ImGuiMouseCursor_ResizeNS,   ImGuiMouseCursor_ResizeNS,
            ImGuiMouseCursor_ResizeAll,  ImGuiMouseCursor_Hand};
        ImGui::SetMouseCursor(cursors[hot]);
    }

    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    auto mouseAngle = [&] { return std::atan2(m.y - c.y, m.x - c.x) * 180.0f / kPi; };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hot >= 0) {
        drag_ = hot;
        grab_[0] = rc.l, grab_[1] = rc.r, grab_[2] = rc.t, grab_[3] = rc.b;
        grab_[4] = node_->paramF(crop::Angle);
        grab_[5] = mouseAngle();
        grabX_ = mu, grabY_ = mv;
    }
    if (drag_ < 0 || !active) return hovered;

    float l = grab_[0], r = grab_[1], t = grab_[2], bt = grab_[3];
    const float minF = 0.02f;
    const float u = std::clamp(mu, 0.0f, 1.0f), v = std::clamp(mv, 0.0f, 1.0f);
    // Height fraction for a width fraction at the locked aspect ratio (and back).
    auto hFor = [&](float wf) { return wf * W / (ar * H); };
    auto wFor = [&](float hf) { return hf * H * ar / W; };
    if (drag_ <= 3) {
        // Corner: the opposite corner stays put.
        const bool right = drag_ == 1 || drag_ == 2, bottom = drag_ == 2 || drag_ == 3;
        const float ax = right ? grab_[0] : grab_[1], ay = bottom ? grab_[2] : grab_[3];
        float wf = std::max(std::fabs(u - ax), minF), hf = std::max(std::fabs(v - ay), minF);
        if (ar > 0) {
            if (wf * W / (hf * H) > ar) wf = wFor(hf);
            else hf = hFor(wf);
            const float maxW = right ? 1 - ax : ax, maxH = bottom ? 1 - ay : ay;
            const float s = std::min({1.0f, maxW / wf, maxH / hf});
            wf *= s, hf *= s;
        }
        if (right) l = ax, r = ax + wf;
        else r = ax, l = ax - wf;
        if (bottom) t = ay, bt = ay + hf;
        else bt = ay, t = ay - hf;
    } else if (drag_ <= 7) {
        // Edge: move that side; with a locked aspect the other axis grows around its centre.
        if (drag_ == 4) l = std::min(u, r - minF);
        if (drag_ == 5) r = std::max(u, l + minF);
        if (drag_ == 6) t = std::min(v, bt - minF);
        if (drag_ == 7) bt = std::max(v, t + minF);
        if (ar > 0) {
            if (drag_ <= 5) {
                float hf = hFor(r - l);
                if (hf > 1) {
                    hf = 1;
                    const float wf = wFor(hf);
                    if (drag_ == 4) l = r - wf;
                    else r = l + wf;
                }
                const float cy = std::clamp((grab_[2] + grab_[3]) * 0.5f, hf * 0.5f, 1 - hf * 0.5f);
                t = cy - hf * 0.5f, bt = cy + hf * 0.5f;
            } else {
                float wf = wFor(bt - t);
                if (wf > 1) {
                    wf = 1;
                    const float hf = hFor(wf);
                    if (drag_ == 6) t = bt - hf;
                    else bt = t + hf;
                }
                const float cx = std::clamp((grab_[0] + grab_[1]) * 0.5f, wf * 0.5f, 1 - wf * 0.5f);
                l = cx - wf * 0.5f, r = cx + wf * 0.5f;
            }
        }
    } else if (drag_ == 8) {
        const float du = std::clamp(mu - grabX_, -grab_[0], 1 - grab_[1]);
        const float dv = std::clamp(mv - grabY_, -grab_[2], 1 - grab_[3]);
        l += du, r += du, t += dv, bt += dv;
    } else {
        // Straighten: turning the mouse around the image centre turns the image with it.
        float d = mouseAngle() - grab_[5];
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        setParam(crop::Angle, grab_[4] + d);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "Angle %.1f", node_->paramF(crop::Angle));
        label(dl, ImVec2(m.x + 16, m.y + 12), buf);
        return true;
    }
    setParam(crop::Left, std::clamp(l, 0.0f, 1.0f));
    setParam(crop::Right, std::clamp(r, 0.0f, 1.0f));
    setParam(crop::Top, std::clamp(t, 0.0f, 1.0f));
    setParam(crop::Bottom, std::clamp(bt, 0.0f, 1.0f));
    return true;
}

// ---------------------------------------------------------------- linear gradient

bool NodeOverlay::updateLinear(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    const float W = b.x - a.x, H = b.y - a.y;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const ImVec2 s(a.x + node_->paramF(0) * W, a.y + node_->paramF(1) * H);
    const ImVec2 e(a.x + node_->paramF(2) * W, a.y + node_->paramF(3) * H);
    const ImVec2 mid((s.x + e.x) * 0.5f, (s.y + e.y) * 0.5f);
    float dx = e.x - s.x, dy = e.y - s.y;
    const float len = std::max(std::hypot(dx, dy), 1e-3f);
    const float nx = -dy / len * (W + H) * 2, ny = dx / len * (W + H) * 2;  // long lines across the image
    outlinedLine(dl, ImVec2(s.x - nx, s.y - ny), ImVec2(s.x + nx, s.y + ny), IM_COL32(255, 255, 255, 235));
    outlinedLine(dl, ImVec2(e.x - nx, e.y - ny), ImVec2(e.x + nx, e.y + ny), IM_COL32(255, 255, 255, 140), 1.0f);
    dl->AddLine(s, e, IM_COL32(255, 255, 255, 160), 1.0f);

    const ImVec2 hp[3] = {s, e, mid};
    int hot = drag_;
    if (hot < 0 && hovered)
        for (int i = 0; i < 3 && hot < 0; ++i)
            if (dist(hp[i], m) < kHit) hot = i;
    for (int i = 0; i < 3; ++i) drawHandle(dl, hp[i], hot == i);
    if (hot >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    const float mu = (m.x - a.x) / W, mv = (m.y - a.y) / H;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hot >= 0) {
        drag_ = hot;
        for (int i = 0; i < 4; ++i) grab_[i] = node_->paramF(i);
        grabX_ = mu, grabY_ = mv;
    }
    if (drag_ < 0 || !active) return hot >= 0;
    if (drag_ == 0) setParam(0, mu), setParam(1, mv);
    if (drag_ == 1) setParam(2, mu), setParam(3, mv);
    if (drag_ == 2) {
        const float du = mu - grabX_, dv = mv - grabY_;
        setParam(0, grab_[0] + du), setParam(1, grab_[1] + dv), setParam(2, grab_[2] + du), setParam(3, grab_[3] + dv);
    }
    return true;
}

// ---------------------------------------------------------------- shape masks

bool NodeOverlay::updateShape(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    // Params: 0 X, 1 Y, 2 Width, 3 Height, 4 Rotation, 5 Feather (see ShapeMaskNode).
    const float W = b.x - a.x, H = b.y - a.y;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const ImVec2 c(a.x + node_->paramF(0) * W, a.y + node_->paramF(1) * H);
    const float hw = node_->paramF(2) * W * 0.5f, hh = node_->paramF(3) * H * 0.5f;
    const float rot = node_->paramF(4) * kPi / 180.0f;
    // The mask's local axes on screen (the node rotates clockwise for positive angles, y down).
    const ImVec2 ex(std::cos(rot), std::sin(rot)), ey(-std::sin(rot), std::cos(rot));
    auto at = [&](float lx, float ly) { return ImVec2(c.x + ex.x * lx + ey.x * ly, c.y + ex.y * lx + ey.y * ly); };
    const bool ellipse = node_->info().type != kBox;
    auto outline = [&](float k, ImU32 col, float th) {
        ImVec2 pts[64];
        int n = 0;
        if (ellipse) {
            for (; n < 64; ++n) {
                const float t = 2 * kPi * n / 64;
                pts[n] = at(std::cos(t) * hw * k, std::sin(t) * hh * k);
            }
        } else {
            pts[n++] = at(-hw * k, -hh * k), pts[n++] = at(hw * k, -hh * k);
            pts[n++] = at(hw * k, hh * k), pts[n++] = at(-hw * k, hh * k);
        }
        outlinedPoly(dl, pts, n, col, th);
    };
    outline(1.0f, IM_COL32(255, 255, 255, 235), 1.5f);
    const float feather = node_->paramF(5);
    if (feather > 0.01f && feather < 0.99f) outline(1.0f - feather, IM_COL32(255, 255, 255, 110), 1.0f);

    const ImVec2 rotHandle = at(0, -hh - 24.0f);
    dl->AddLine(at(0, -hh), rotHandle, IM_COL32(255, 255, 255, 140));
    const ImVec2 hp[4] = {c, at(hw, 0), at(0, hh), rotHandle};
    int hot = drag_;
    if (hot < 0 && hovered)
        for (int i = 3; i >= 0 && hot < 0; --i)  // small handles first: the centre may overlap them
            if (dist(hp[i], m) < kHit) hot = i;
    for (int i = 0; i < 4; ++i) drawHandle(dl, hp[i], hot == i);
    if (hot >= 0) ImGui::SetMouseCursor(hot == 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
    const float mu = (m.x - a.x) / W, mv = (m.y - a.y) / H;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hot >= 0) {
        drag_ = hot;
        grabX_ = mu - node_->paramF(0), grabY_ = mv - node_->paramF(1);
    }
    if (drag_ < 0 || !active) return hot >= 0;
    const float dx = m.x - c.x, dy = m.y - c.y;
    switch (drag_) {
        case 0: setParam(0, mu - grabX_), setParam(1, mv - grabY_); break;
        case 1: setParam(2, 2.0f * std::fabs(dx * ex.x + dy * ex.y) / W); break;
        case 2: setParam(3, 2.0f * std::fabs(dx * ey.x + dy * ey.y) / H); break;
        case 3: {
            float deg = std::atan2(dx, -dy) * 180.0f / kPi;
            if (ImGui::GetIO().KeyShift) deg = std::round(deg / 15.0f) * 15.0f;  // snap like Blender
            setParam(4, deg);
            break;
        }
    }
    return true;
}

// ---------------------------------------------------------------- brush

bool NodeOverlay::updateBrush(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    auto* brush = static_cast<BrushMaskNode*>(node_);
    const float W = b.x - a.x, H = b.y - a.y;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const float mu = (m.x - a.x) / W, mv = (m.y - a.y) / H;
    if (hovered) {
        // [ and ] resize the brush, as in Lightroom and Photoshop.
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) setParam(0, brush->paramF(0) / 1.25f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) setParam(0, brush->paramF(0) * 1.25f);
        const float r = brush->paramF(0) * std::max(W, H);
        const bool erase = io.KeyAlt || (drag_ >= 0 && !brush->strokes.empty() && brush->strokes.back().erase);
        const ImU32 col = erase ? IM_COL32(255, 120, 120, 230) : IM_COL32(255, 255, 255, 230);
        dl->AddCircle(m, r, IM_COL32(0, 0, 0, 150), 0, 3.0f);
        dl->AddCircle(m, r, col, 0, 1.5f);
        const float inner = r * (1.0f - brush->paramF(1));
        if (inner > 2.0f && inner < r - 2.0f) dl->AddCircle(m, inner, IM_COL32(255, 255, 255, 110), 0, 1.0f);
        dl->AddLine(ImVec2(m.x - 4, m.y), ImVec2(m.x + 4, m.y), col);  // "-" when erasing, "+" when painting
        if (!erase) dl->AddLine(ImVec2(m.x, m.y - 4), ImVec2(m.x, m.y + 4), col);
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            brush->beginStroke(mu, mv, io.KeyAlt);
            drag_ = 0;
            changed_ = true;
        }
    }
    if (drag_ >= 0 && active && brush->extendStroke(mu, mv, std::max(1, int(W)), std::max(1, int(H)))) changed_ = true;
    return hovered || drag_ >= 0;
}

// ---------------------------------------------------------------- spot removal

bool NodeOverlay::updateSpots(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    auto* node = static_cast<SpotRemovalNode*>(node_);
    std::vector<Spot>& spots = node->spots;
    if (node->active >= int(spots.size())) node->active = -1;
    const float W = b.x - a.x, H = b.y - a.y, L = std::max(W, H);
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    auto screen = [&](float u, float v) { return ImVec2(a.x + u * W, a.y + v * H); };

    // The Inspector edits the active spot through the params.
    if (node->storeActive()) changed_ = true;
    // A spot added with a plain click gets an automatic source, as in Lightroom (the App finds
    // it); dragging right away set it by hand.
    if (drag_ < 0 && spotClick_) {
        spotClick_ = false;
        if (node->active >= 0) node->findSource = node->active, node->findAvoidCurrent = false;
    }

    // What is under the mouse: the active spot's parts first, then any spot's target.
    enum Part { None, Target, Source, Edge };
    int hitSpot = -1;
    Part hitPart = None;
    if (hovered && drag_ < 0) {
        auto test = [&](int i, bool withSource) {
            const Spot& s = spots[i];
            const float r = s.radius * L, dt = dist(m, screen(s.x, s.y));
            if (std::fabs(dt - r) <= kHit * 0.6f) return Edge;
            if (dt < r) return Target;
            if (withSource && dist(m, screen(s.sx, s.sy)) < r) return Source;
            return None;
        };
        if (node->active >= 0 && (hitPart = test(node->active, true)) != None) hitSpot = node->active;
        for (int i = int(spots.size()) - 1; i >= 0 && hitSpot < 0; --i)
            if (Part p = test(i, false); p != None) hitSpot = i, hitPart = p == Edge ? Target : p;
    }

    if (hovered) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) setParam(1, node->paramF(1) / 1.25f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) setParam(1, node->paramF(1) * 1.25f);
        // "/" looks for another source, as in Lightroom.
        if (node->active >= 0 && ImGui::IsKeyPressed(ImGuiKey_Slash)) node->findSource = node->active, node->findAvoidCurrent = true;
        if (node->active >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
            spots.erase(spots.begin() + node->active);
            node->active = -1;
            changed_ = true;
            hitSpot = -1, hitPart = None;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (hitSpot >= 0 && io.KeyAlt) {
                // Alt+click removes a spot, as in Lightroom.
                spots.erase(spots.begin() + hitSpot);
                node->active = -1;
                changed_ = true;
            } else if (hitSpot >= 0) {
                if (node->active != hitSpot) {
                    node->active = hitSpot;
                    node->loadActive();
                    changed_ = true;
                }
                const Spot& s = spots[hitSpot];
                drag_ = hitPart == Source ? 1 : hitPart == Edge ? 2 : 0;
                grab_[0] = s.x, grab_[1] = s.y, grab_[2] = s.sx, grab_[3] = s.sy, grab_[4] = s.radius;
                grabX_ = m.x, grabY_ = m.y;
            } else if (!io.KeyAlt && m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) {
                node->addSpot((m.x - a.x) / W, (m.y - a.y) / H, W / H);
                changed_ = true;
                spotClick_ = true;
                // Dragging right after adding moves the source, as Photoshop's healing brush
                // sets it; a plain click keeps the one picked beside it.
                const Spot& s = spots.back();
                drag_ = 3;
                grab_[2] = s.sx, grab_[3] = s.sy;
                grabX_ = m.x, grabY_ = m.y;
            }
        }
    }

    if (drag_ >= 0 && active && node->active >= 0) {
        Spot& s = spots[node->active];
        const float du = (m.x - grabX_) / W, dv = (m.y - grabY_) / H;
        const Spot before = s;
        if (drag_ == 0) s.x = grab_[0] + du, s.y = grab_[1] + dv;
        else if (drag_ == 1) s.sx = grab_[2] + du, s.sy = grab_[3] + dv;
        else if (drag_ == 2) setParam(1, dist(m, screen(s.x, s.y)) / L);
        else if (drag_ == 3 && dist(m, ImVec2(grabX_, grabY_)) > 4.0f) s.sx = (m.x - a.x) / W, s.sy = (m.y - a.y) / H, spotClick_ = false;
        if (drag_ == 2 && node->storeActive()) changed_ = true;
        if (s.x != before.x || s.y != before.y || s.sx != before.sx || s.sy != before.sy) changed_ = true;
    }

    // Spots: the active one with its source and an arrow from it, the others as plain circles.
    for (int i = 0; i < int(spots.size()); ++i) {
        const Spot& s = spots[i];
        const ImVec2 t = screen(s.x, s.y);
        const float r = std::max(s.radius * L, 2.0f);
        const bool isActive = i == node->active, hot = i == hitSpot;
        const ImU32 col = isActive ? IM_COL32(255, 255, 255, 240) : hot ? IM_COL32(255, 220, 140, 230) : IM_COL32(220, 220, 225, 150);
        dl->AddCircle(t, r, IM_COL32(0, 0, 0, 140), 0, isActive ? 3.5f : 2.5f);
        dl->AddCircle(t, r, col, 0, isActive ? 1.5f : 1.0f);
        if (!isActive) continue;
        const ImVec2 sp = screen(s.sx, s.sy);
        const ImU32 scol = hot && hitPart == Source ? IM_COL32(255, 220, 140, 230) : IM_COL32(200, 200, 205, 200);
        dl->AddCircle(sp, r, IM_COL32(0, 0, 0, 120), 0, 2.5f);
        dl->AddCircle(sp, r, scol, 0, 1.0f);
        const float d = dist(t, sp);
        if (d > 2 * r + 6) {
            // From the source's edge to the target's, pointing where the pixels go.
            const ImVec2 dir((t.x - sp.x) / d, (t.y - sp.y) / d);
            const ImVec2 p0(sp.x + dir.x * r, sp.y + dir.y * r), p1(t.x - dir.x * r, t.y - dir.y * r);
            outlinedLine(dl, p0, p1, scol, 1.0f);
            const ImVec2 n(-dir.y, dir.x);
            dl->AddTriangleFilled(p1, ImVec2(p1.x - dir.x * 8 + n.x * 4, p1.y - dir.y * 8 + n.y * 4),
                                  ImVec2(p1.x - dir.x * 8 - n.x * 4, p1.y - dir.y * 8 - n.y * 4), scol);
        }
        // Top right of the circle, or top left where that would leave the view.
        const char* mode = s.heal ? "Heal" : "Clone";
        const float tw = ImGui::CalcTextSize(mode).x;
        const float lx = t.x + r * 0.72f + 4 + tw + 4 > dl->GetClipRectMax().x ? t.x - r * 0.72f - 4 - tw : t.x + r * 0.72f + 4;
        label(dl, ImVec2(lx, t.y - r * 0.72f - 16), mode);
    }

    // Over empty image: the size a new spot would have.
    if (hovered && hitSpot < 0 && drag_ < 0) {
        const float r = node->paramF(1) * L;
        dl->AddCircle(m, r, IM_COL32(0, 0, 0, 120), 0, 2.5f);
        dl->AddCircle(m, r, IM_COL32(255, 255, 255, 170), 0, 1.0f);
    }
    if (hovered && (hitPart == Target || hitPart == Source) && drag_ < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (hovered && hitPart == Edge && drag_ < 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    return hovered || drag_ >= 0;
}

// ---------------------------------------------------------------- perspective

bool NodeOverlay::updatePerspective(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    auto* pn = static_cast<PerspectiveNode*>(node_);
    const float W = b.x - a.x, H = b.y - a.y;
    gridLines(dl, a, b, 16, IM_COL32(255, 255, 255, 60));
    // A guide just drawn and released without length is dropped.
    if (drag_ < 0 && creating_) {
        creating_ = false;
        if (!pn->guides.empty()) {
            const perspective::Guide& g = pn->guides.back();
            if (std::hypot((g.x1 - g.x0) * W, (g.y1 - g.y0) * H) < 4.0f) {
                pn->guides.pop_back();
                changed_ = true;
            }
        }
    }
    if (pn->paramI(perspective::Upright) != perspective::UprightGuided) return false;

    // Guides are stored in the node's input; the viewer shows its output. Only the shape of
    // the image matters to the transform, so any size with the view's aspect does.
    if (drag_ < 0) {
        const int iw = std::max(1, int(std::lround(W * 8))), ih = std::max(1, int(std::lround(H * 8)));
        toInput_ = pn->matrix(iw, ih);
        toShown_ = perspective::inverse(toInput_);
    }
    const ImVec2 m = ImGui::GetIO().MousePos;
    const auto shown = [&](float u, float v, ImVec2& out) {
        double su, sv;
        if (!perspective::apply(toShown_, u, v, su, sv)) return false;
        out = ImVec2(a.x + float(su) * W, a.y + float(sv) * H);
        return true;
    };
    const auto input = [&](const ImVec2& p, float& u, float& v) {
        double iu, iv;
        if (!perspective::apply(toInput_, (p.x - a.x) / W, (p.y - a.y) / H, iu, iv)) return false;
        u = std::clamp(float(iu), 0.0f, 1.0f), v = std::clamp(float(iv), 0.0f, 1.0f);
        return true;
    };

    // Handles: guide g's ends are 2g and 2g+1. A guide's line is hot for Alt+click removal.
    const int n = int(pn->guides.size());
    const float aspect = W / std::max(H, 1.0f);
    int hot = drag_, hotLine = -1;
    std::vector<ImVec2> ends(size_t(n) * 2);
    std::vector<bool> visible(size_t(n), false);
    for (int g = 0; g < n; ++g) {
        const perspective::Guide& gd = pn->guides[size_t(g)];
        visible[size_t(g)] = shown(gd.x0, gd.y0, ends[size_t(g) * 2]) && shown(gd.x1, gd.y1, ends[size_t(g) * 2 + 1]);
    }
    if (hot < 0 && hovered)
        for (int g = 0; g < n && hot < 0; ++g) {
            if (!visible[size_t(g)]) continue;
            for (int e = 0; e < 2 && hot < 0; ++e)
                if (dist(ends[size_t(g) * 2 + e], m) < kHit) hot = g * 2 + e;
            // Distance to the segment.
            const ImVec2 p = ends[size_t(g) * 2], q = ends[size_t(g) * 2 + 1];
            const float dx = q.x - p.x, dy = q.y - p.y, len2 = std::max(dx * dx + dy * dy, 1e-6f);
            const float t = std::clamp(((m.x - p.x) * dx + (m.y - p.y) * dy) / len2, 0.0f, 1.0f);
            if (hotLine < 0 && dist(ImVec2(p.x + dx * t, p.y + dy * t), m) < 5.0f) hotLine = g;
        }
    for (int g = 0; g < n; ++g) {
        if (!visible[size_t(g)]) continue;
        const bool vert = pn->guides[size_t(g)].vertical(aspect);
        const bool hotG = hot / 2 == g || hotLine == g;
        // Steep guides (they'll become vertical) and flat ones (horizontal) in different colours.
        const ImU32 col = hotG ? IM_COL32(255, 200, 80, 255) : vert ? IM_COL32(120, 210, 255, 240) : IM_COL32(255, 140, 200, 240);
        outlinedLine(dl, ends[size_t(g) * 2], ends[size_t(g) * 2 + 1], col);
        drawHandle(dl, ends[size_t(g) * 2], hot == g * 2);
        drawHandle(dl, ends[size_t(g) * 2 + 1], hot == g * 2 + 1);
    }
    const bool alt = ImGui::GetIO().KeyAlt;
    const bool inside = m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y;
    if (hovered && drag_ < 0) {
        if (hot >= 0 || hotLine >= 0) ImGui::SetMouseCursor(alt ? ImGuiMouseCursor_NotAllowed : ImGuiMouseCursor_ResizeAll);
        else if (inside && n < perspective::kMaxGuides) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int g = hot >= 0 ? hot / 2 : hotLine;
        if (alt && g >= 0) {
            // Alt+click removes a guide, as in Lightroom.
            pn->guides.erase(pn->guides.begin() + g);
            changed_ = true;
            return true;
        }
        if (hot >= 0) {
            drag_ = hot;
        } else if (inside && !alt && n < perspective::kMaxGuides) {
            float u, v;
            if (input(m, u, v)) {
                pn->guides.push_back({u, v, u, v});
                drag_ = n * 2 + 1;
                creating_ = true;
                changed_ = true;
            }
        }
    }
    if (drag_ < 0 || !active || drag_ / 2 >= int(pn->guides.size())) return hovered && (hot >= 0 || hotLine >= 0 || inside);
    float u, v;
    if (input(m, u, v)) {
        perspective::Guide& gd = pn->guides[size_t(drag_ / 2)];
        float& gu = drag_ % 2 ? gd.x1 : gd.x0;
        float& gv = drag_ % 2 ? gd.y1 : gd.y0;
        if (gu != u || gv != v) gu = u, gv = v, changed_ = true;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Guide %d of %d", drag_ / 2 + 1, perspective::kMaxGuides);
    label(dl, ImVec2(m.x + 16, m.y + 12), buf);
    return true;
}

// ---------------------------------------------------------------- loupe overlay

bool LoupeOverlay::update(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active) {
    if (b.x - a.x < 2 || b.y - a.y < 2) return inner && inner->update(dl, a, b, hovered, active);
    dl->PushClipRect(a, b, true);
    if (grid) {
        // Anchored to the image, so it moves with it when panning.
        const float s = std::max(gridSize, 4.0f);
        const ImVec2 lo = dl->GetClipRectMin(), hi = dl->GetClipRectMax();
        const ImU32 col = IM_COL32(255, 255, 255, 70);
        for (float x = a.x + std::max(0.0f, std::floor((lo.x - a.x) / s)) * s; x <= std::min(b.x, hi.x); x += s)
            dl->AddLine(ImVec2(x, std::max(a.y, lo.y)), ImVec2(x, std::min(b.y, hi.y)), col);
        for (float y = a.y + std::max(0.0f, std::floor((lo.y - a.y) / s)) * s; y <= std::min(b.y, hi.y); y += s)
            dl->AddLine(ImVec2(std::max(a.x, lo.x), y), ImVec2(std::min(b.x, hi.x), y), col);
    }
    if (composition) {
        const int alpha = int(std::clamp(opacity, 0.05f, 1.0f) * 255.0f + 0.5f);
        if (guide >= NodeOverlay::kCropGuideCount && guide < guideCount())
            drawCustomGuide(dl, a, b, custom[guide - NodeOverlay::kCropGuideCount], alpha);
        else drawCropGuide(dl, a, b, std::clamp(guide, 0, NodeOverlay::kCropGuideCount - 1), turn, alpha);
    }
    const ImVec2 g(a.x + guideX * (b.x - a.x), a.y + guideY * (b.y - a.y));
    if (guides) {
        const bool hotV = drag_ == 0 || drag_ == 2, hotH = drag_ == 1 || drag_ == 2;
        outlinedLine(dl, ImVec2(g.x, a.y), ImVec2(g.x, b.y), hotV ? IM_COL32(255, 200, 80, 255) : IM_COL32(255, 255, 255, 200), 1.0f);
        outlinedLine(dl, ImVec2(a.x, g.y), ImVec2(b.x, g.y), hotH ? IM_COL32(255, 200, 80, 255) : IM_COL32(255, 255, 255, 200), 1.0f);
    }
    dl->PopClipRect();

    // The node's controls come first (and only draw while a guide is dragged).
    const bool innerOwns = inner && inner->update(dl, a, b, hovered && drag_ < 0, active && drag_ < 0);
    if (drag_ < 0 && innerOwns) return true;
    if (!guides) return innerOwns;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_ = -1;
    const ImVec2 m = ImGui::GetIO().MousePos;
    int hot = drag_;
    if (hot < 0 && hovered) {
        const bool nearV = std::fabs(m.x - g.x) < 5.0f && m.y >= a.y && m.y <= b.y;
        const bool nearH = std::fabs(m.y - g.y) < 5.0f && m.x >= a.x && m.x <= b.x;
        hot = nearV && nearH ? 2 : nearV ? 0 : nearH ? 1 : -1;
    }
    if (hot >= 0) ImGui::SetMouseCursor(hot == 2 ? ImGuiMouseCursor_ResizeAll : hot == 0 ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (hovered && hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drag_ = hot;
    if (drag_ < 0 || !active) return hot >= 0;
    if (drag_ != 1) guideX = std::clamp((m.x - a.x) / (b.x - a.x), 0.0f, 1.0f);
    if (drag_ != 0) guideY = std::clamp((m.y - a.y) / (b.y - a.y), 0.0f, 1.0f);
    return true;
}
