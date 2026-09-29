#include "ui/ImageView.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

GLTexture::~GLTexture() { reset(); }

void GLTexture::reset() {
    if (id_) {
        GLuint t = id_;
        glDeleteTextures(1, &t);
    }
    id_ = 0;
    w_ = h_ = 0;
}

void GLTexture::upload(const Image& img) {
    if (img.empty()) {
        reset();
        return;
    }
    std::vector<unsigned char> bytes(img.px.size());
    for (size_t i = 0; i < bytes.size(); i += 4) {
        for (int k = 0; k < 3; ++k)
            bytes[i + k] = static_cast<unsigned char>(std::lround(std::clamp(img.px[i + k], 0.0f, 1.0f) * 255.0f));
        bytes[i + 3] = 255;  // show alpha as opaque; transparency display comes later
    }
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
    w_ = img.w;
    h_ = img.h;
}

void drawImageView(const char* id, const GLTexture& tex, ViewState& view, const char* emptyText) {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, 1.0f);
    avail.y = std::max(avail.y, 1.0f);

    ImGui::InvisibleButton(id, avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(origin.x + avail.x, origin.y + avail.y);
    dl->AddRectFilled(origin, end, IM_COL32(24, 24, 27, 255));

    if (!tex.valid()) {
        ImVec2 ts = ImGui::CalcTextSize(emptyText);
        dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + (avail.y - ts.y) * 0.5f),
                    IM_COL32(140, 140, 150, 255), emptyText);
        return;
    }

    const float fit = std::min(avail.x / tex.width(), avail.y / tex.height());
    const ImVec2 center(origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f);

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0.0f) {
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
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) view.reset();
    }
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        view.panX += io.MouseDelta.x / view.zoom;
        view.panY += io.MouseDelta.y / view.zoom;
    }

    const float scale = fit * view.zoom;
    const float hw = tex.width() * scale * 0.5f, hh = tex.height() * scale * 0.5f;
    const float cx = center.x + view.panX * view.zoom, cy = center.y + view.panY * view.zoom;
    dl->PushClipRect(origin, end, true);
    dl->AddImage((ImTextureID)(intptr_t)tex.id(), ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh));
    dl->PopClipRect();
}
