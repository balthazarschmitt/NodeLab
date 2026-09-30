#include "ui/ViewerOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

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

}  // namespace

bool NodeOverlay::supports(const Node& n) {
    const std::string& t = n.info().type;
    return t == crop::kType || isMask(n);
}

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
    if (t == kLinear) return updateLinear(dl, a, b, hovered, active);
    if (t == kRadial || t == kBox || t == kEllipse) return updateShape(dl, a, b, hovered, active);
    if (dynamic_cast<BrushMaskNode*>(node_)) return updateBrush(dl, a, b, hovered, active);
    return false;
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

    // Dim what will be cut off, then the frame and rule-of-thirds grid.
    const ImU32 dim = IM_COL32(0, 0, 0, 150);
    dl->AddRectFilled(a, ImVec2(b.x, p0.y), dim);
    dl->AddRectFilled(ImVec2(a.x, p1.y), b, dim);
    dl->AddRectFilled(ImVec2(a.x, p0.y), ImVec2(p0.x, p1.y), dim);
    dl->AddRectFilled(ImVec2(p1.x, p0.y), ImVec2(b.x, p1.y), dim);
    const int lines = drag_ == 9 ? 9 : 3;  // a finer grid while straightening, to line up horizons
    for (int i = 1; i < lines; ++i) {
        const float fx = p0.x + (p1.x - p0.x) * i / lines, fy = p0.y + (p1.y - p0.y) * i / lines;
        dl->AddLine(ImVec2(fx, p0.y), ImVec2(fx, p1.y), IM_COL32(255, 255, 255, 90));
        dl->AddLine(ImVec2(p0.x, fy), ImVec2(p1.x, fy), IM_COL32(255, 255, 255, 90));
    }
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
