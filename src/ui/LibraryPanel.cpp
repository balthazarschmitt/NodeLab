#include "ui/LibraryPanel.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "graph/Graph.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"

namespace {

// Lightroom's Library Filter, reduced to the attributes NodeLab keeps.
const char* const kFilters[] = {"All Photos", "Picked", "Hide Rejected", "Rejected",
                                "1 Star or More", "2 Stars or More", "3 Stars or More",
                                "4 Stars or More", "5 Stars", "Edited"};

const ImU32 kAccent = IM_COL32(90, 150, 255, 255);

// A five-pointed star, as Lightroom's grid shows ratings.
void drawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled) {
    ImVec2 pts[10];
    for (int k = 0; k < 10; ++k) {
        const float a = -1.5707963f + k * 0.62831853f, rr = k % 2 ? r * 0.45f : r;
        pts[k] = ImVec2(c.x + rr * std::cos(a), c.y + rr * std::sin(a));
    }
    if (filled) dl->AddConcavePolyFilled(pts, 10, col);
    else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.0f);
}

}  // namespace

LibraryPanel::~LibraryPanel() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool LibraryPanel::open(const std::string& dirU8) {
    std::vector<std::string> photos = library::listFolder(dirU8);
    if (photos.empty()) return false;
    {
        std::lock_guard lock(mutex_);
        ++gen_;
        jobs_.clear();
        results_.clear();
    }
    dir_ = dirU8;
    items_.clear();
    for (std::string& p : photos) {
        auto it = std::make_unique<Item>();
        it->path = std::move(p);
        library::readMeta(it->path, it->meta);
        items_.push_back(std::move(it));
    }
    current_ = anchor_ = -1;
    if (!thread_.joinable()) thread_ = std::thread([this] { work(); });
    return true;
}

void LibraryPanel::setCurrentProject(const std::string& projectU8) {
    int found = -1;
    for (int i = 0; i < size(); ++i)
        if (library::sidecarPath(photo(i)) == projectU8) found = i;
    if (found == current_) return;
    current_ = found;
    if (found < 0) return;
    // Opening a photo selects it alone, unless it is part of the selection already.
    if (!items_[size_t(found)]->selected)
        for (int i = 0; i < size(); ++i) items_[size_t(i)]->selected = i == found;
    anchor_ = found;
    scrollToCurrent_ = true;
}

std::vector<int> LibraryPanel::selection() const {
    std::vector<int> out;
    for (int i = 0; i < size(); ++i)
        if (items_[size_t(i)]->selected && passes(*items_[size_t(i)])) out.push_back(i);
    if (out.empty() && current_ >= 0) out.push_back(current_);
    return out;
}

void LibraryPanel::refresh(int i, bool rerender) {
    if (i < 0 || i >= size()) return;
    Item& it = *items_[size_t(i)];
    library::Meta m;
    if (library::readMeta(it.path, m)) {
        if (rerender) m.thumb.clear();
        it.meta = m;
    }
    request(i, rerender);
}

int LibraryPanel::step(int from, int dir) const {
    for (int i = from + dir; i >= 0 && i < size(); i += dir)
        if (passes(*items_[size_t(i)])) return i;
    return -1;
}

bool LibraryPanel::passes(const Item& it) const {
    const library::Meta& m = it.meta;
    switch (filter_) {
        case 1: return m.flag == library::Picked;
        case 2: return m.flag != library::Rejected;
        case 3: return m.flag == library::Rejected;
        case 4: case 5: case 6: case 7: case 8: return m.rating >= filter_ - 3;
        case 9: return m.edited;
        default: return true;
    }
}

void LibraryPanel::writeMeta(int i) {
    std::string err;
    if (!library::writeMeta(photo(i), meta(i), err)) status = "Could not save the rating: " + err;
}

void LibraryPanel::setRating(int rating) {
    const std::vector<int> sel = selection();
    for (int i : sel) {
        meta(i).rating = rating;
        writeMeta(i);
    }
    if (!sel.empty()) status = rating ? "Rated " + std::to_string(rating) + (rating == 1 ? " star" : " stars") : "Rating cleared";
}

void LibraryPanel::setFlag(int flag) {
    const std::vector<int> sel = selection();
    for (int i : sel) {
        meta(i).flag = flag;
        writeMeta(i);
    }
    if (!sel.empty()) status = flag == library::Picked ? "Flagged as Pick" : flag == library::Rejected ? "Flagged as Rejected" : "Flag removed";
}

void LibraryPanel::request(int i, bool render) {
    Item& it = *items_[size_t(i)];
    it.requested = true;
    std::lock_guard lock(mutex_);
    // Newest requests first: what scrolled into view, or the edit just saved.
    jobs_.push_front(Job{i, gen_, it.path, it.meta, render});
    wake_.notify_one();
}

void LibraryPanel::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Result r{job.index, job.gen, nullptr, {}};
        std::string err;
        // An edited photo shows its edit: stored in the sidecar, or rendered (and then stored).
        if (job.render || (job.meta.edited && job.meta.thumb.empty() && library::hasSidecar(job.path))) {
            Graph g;
            nlohmann::json ui;
            if (loadProject(library::sidecarPath(job.path), g, ui, err))
                if ((r.image = library::renderThumbnail(g, library::kThumbEdge, err))) r.store = library::encodeThumb(*r.image);
        } else if (!job.meta.thumb.empty()) {
            r.image = library::decodeThumb(job.meta.thumb);
        }
        if (!r.image) r.image = library::loadThumbnail(job.path, library::kThumbEdge, err);
        std::lock_guard lock(mutex_);
        if (job.gen == gen_) results_.push_back(std::move(r));
    }
}

void LibraryPanel::poll() {
    std::deque<Result> done;
    {
        std::lock_guard lock(mutex_);
        done.swap(results_);
    }
    for (Result& r : done) {
        if (r.index < 0 || r.index >= size() || !r.image) continue;
        Item& it = *items_[size_t(r.index)];
        it.tex.upload(*r.image);
        if (!r.store.empty()) {
            it.meta.thumb = std::move(r.store);
            writeMeta(r.index);
        }
    }
}

void LibraryPanel::cullKeys() {
    // Lightroom's culling keys: 0-5 rate, P pick, X reject, U unflag.
    for (int r = 0; r <= 5; ++r)
        if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_0 + r), false) || ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad0 + r), false))
            setRating(r);
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) setFlag(library::Picked);
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) setFlag(library::Rejected);
    if (ImGui::IsKeyPressed(ImGuiKey_U, false)) setFlag(library::Unflagged);
}

void LibraryPanel::select(int i, bool ctrl, bool shift) {
    if (ctrl) {
        items_[size_t(i)]->selected = !items_[size_t(i)]->selected;
        anchor_ = i;
    } else if (shift && anchor_ >= 0) {
        const int lo = std::min(anchor_, i), hi = std::max(anchor_, i);
        for (int k = 0; k < size(); ++k) items_[size_t(k)]->selected = k >= lo && k <= hi && passes(*items_[size_t(k)]);
    } else {
        for (int k = 0; k < size(); ++k) items_[size_t(k)]->selected = k == i;
        anchor_ = i;
    }
}

LibraryPanel::Actions LibraryPanel::draw(bool keys) {
    Actions a;
    ImGuiIO& io = ImGui::GetIO();

    // ---- culling keys, and the arrows step through the photos
    if (keys && !io.KeyCtrl && !io.KeyAlt) {
        cullKeys();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) a.open = step(current_, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) a.open = step(current_, +1);
    }

    toolbar(a);
    ImGui::SameLine();
    if (ImGui::SmallButton("Grid")) {
        grid = true;
        focus_ = current_;
        scrollToFocus_ = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the whole folder as a grid (G)");

    ImGui::BeginChild("##strip", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    return drawStrip(a);
}

void LibraryPanel::toolbar(Actions& a) {
    const std::string name = pathToU8(u8ToPath(dir_).filename());
    int shown = 0;
    for (const auto& it : items_) shown += passes(*it);
    ImGui::TextDisabled("%s", name.empty() ? dir_.c_str() : name.c_str());
    ImGui::SameLine();
    if (current_ >= 0)
        ImGui::TextDisabled("%d / %d   %s", current_ + 1, size(), pathToU8(u8ToPath(photo(current_)).filename()).c_str());
    else
        ImGui::TextDisabled("%d photos", size());
    if (shown != size()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d shown)", shown);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("##filter", &filter_, kFilters, IM_ARRAYSIZE(kFilters))) scrollToCurrent_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Library Filter: which photos the filmstrip shows");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy Edit")) a.copy = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy this photo's edit (Ctrl+Shift+C)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!canPaste);
    if (ImGui::SmallButton("Paste Edit")) a.paste = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Paste the copied edit onto the selected photos (Ctrl+Shift+V)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Export Selected...")) a.exportSelected = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render the selected photos, each with its own edit, with the Export window's settings");
}

LibraryPanel::Actions LibraryPanel::drawStrip(Actions a) {
    ImGuiIO& io = ImGui::GetIO();
    const float scrollbar = ImGui::GetStyle().ScrollbarSize;
    const float cellH = std::max(40.0f, ImGui::GetContentRegionAvail().y - scrollbar);
    const float dotsH = 14.0f;
    const float thumbH = cellH - dotsH, thumbW = thumbH * 1.5f, cellW = thumbW + 8.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f && !io.KeyCtrl)
        ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseWheel * cellW);
    bool first = true;
    for (int i = 0; i < size(); ++i) {
        Item& it = *items_[size_t(i)];
        if (!passes(it)) continue;
        if (!first) ImGui::SameLine(0, 0);
        first = false;
        ImGui::PushID(i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##cell", ImVec2(cellW, cellH));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            select(i, io.KeyCtrl, io.KeyShift);
            if (!io.KeyCtrl && !io.KeyShift && i != current_) a.open = i;
        }
        if (i == current_ && scrollToCurrent_) {
            ImGui::SetScrollHereX(0.5f);
            scrollToCurrent_ = false;
        }
        if (ImGui::IsItemVisible() && !it.requested) request(i, false);

        const ImVec2 b0(p0.x + 4, p0.y + 2), b1(p0.x + 4 + thumbW, p0.y + thumbH - 2);
        drawThumb(dl, it, b0, b1);
        if (i == current_) dl->AddRect(ImVec2(b0.x - 2, b0.y - 2), ImVec2(b1.x + 2, b1.y + 2), IM_COL32_WHITE, 0, 0, 2.0f);
        else if (it.selected) dl->AddRect(ImVec2(b0.x - 2, b0.y - 2), ImVec2(b1.x + 2, b1.y + 2), kAccent, 0, 0, 2.0f);
        else if (hovered) dl->AddRect(b0, b1, IM_COL32(255, 255, 255, 80));
        // Rating: five dots, filled up to the stars given.
        const float cy = p0.y + thumbH + dotsH * 0.5f - 1, cx0 = p0.x + 4 + thumbW * 0.5f - 4 * 9.0f * 0.5f;
        for (int s = 0; s < 5; ++s) {
            const ImVec2 c(cx0 + s * 9.0f, cy);
            if (s < it.meta.rating) dl->AddCircleFilled(c, 3.2f, IM_COL32(235, 235, 235, 255));
            else dl->AddCircle(c, 2.6f, IM_COL32(120, 120, 128, 255));
        }
        if (hovered) {
            ImGui::SetTooltip("%s%s%s", pathToU8(u8ToPath(it.path).filename()).c_str(),
                              library::hasSidecar(it.path) ? "\nEdit in " : "",
                              library::hasSidecar(it.path) ? pathToU8(u8ToPath(library::sidecarPath(it.path)).filename()).c_str() : "");
        }
        ImGui::PopID();
    }
    if (first) ImGui::TextDisabled("No photos match the filter");
    ImGui::EndChild();
    return a;
}

void LibraryPanel::drawThumb(ImDrawList* dl, const Item& it, ImVec2 b0, ImVec2 b1) const {
    // The thumbnail, fitted into its box; rejected photos are dimmed as in Lightroom.
    dl->AddRectFilled(b0, b1, IM_COL32(30, 30, 34, 255));
    if (it.tex.valid()) {
        const float s = std::min((b1.x - b0.x) / it.tex.width(), (b1.y - b0.y) / it.tex.height());
        const float w = it.tex.width() * s, h = it.tex.height() * s;
        const ImVec2 i0(b0.x + (b1.x - b0.x - w) * 0.5f, b0.y + (b1.y - b0.y - h) * 0.5f);
        const ImU32 tint = it.meta.flag == library::Rejected ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;
        dl->AddImage(ImTextureID(intptr_t(it.tex.id())), i0, ImVec2(i0.x + w, i0.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
    }
    // Badges: flag at the top left, an edited corner at the top right.
    if (it.meta.flag == library::Picked) {
        dl->AddRectFilled(ImVec2(b0.x + 5, b0.y + 5), ImVec2(b0.x + 7, b0.y + 19), IM_COL32_WHITE);
        dl->AddTriangleFilled(ImVec2(b0.x + 7, b0.y + 5), ImVec2(b0.x + 17, b0.y + 9), ImVec2(b0.x + 7, b0.y + 13), IM_COL32_WHITE);
    } else if (it.meta.flag == library::Rejected) {
        const ImU32 red = IM_COL32(235, 70, 60, 255);
        dl->AddLine(ImVec2(b0.x + 6, b0.y + 6), ImVec2(b0.x + 16, b0.y + 16), red, 2.5f);
        dl->AddLine(ImVec2(b0.x + 16, b0.y + 6), ImVec2(b0.x + 6, b0.y + 16), red, 2.5f);
    }
    if (it.meta.edited) {
        // Outlined, so it shows on a blue sky too.
        dl->AddTriangleFilled(ImVec2(b1.x - 17, b0.y), ImVec2(b1.x, b0.y), ImVec2(b1.x, b0.y + 17), IM_COL32(0, 0, 0, 160));
        dl->AddTriangleFilled(ImVec2(b1.x - 14, b0.y), ImVec2(b1.x, b0.y), ImVec2(b1.x, b0.y + 14), kAccent);
    }
}

LibraryPanel::Actions LibraryPanel::drawGrid(bool keys) {
    Actions a;
    ImGuiIO& io = ImGui::GetIO();
    // The cells: the photos the filter shows, in order.
    std::vector<int> shown;
    for (int i = 0; i < size(); ++i)
        if (passes(*items_[size_t(i)])) shown.push_back(i);
    auto posOf = [&](int i) { return int(std::find(shown.begin(), shown.end(), i) - shown.begin()); };
    if (focus_ < 0 || focus_ >= size() || !passes(*items_[size_t(focus_)]))
        focus_ = current_ >= 0 && passes(*items_[size_t(current_)]) ? current_ : shown.empty() ? -1 : shown[0];

    toolbar(a);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("##size", &gridSize_, 100.0f, 360.0f, "Size %.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thumbnail size (Ctrl+wheel)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Loupe")) grid = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the editor (Escape; E or a double-click opens a photo)");

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##grid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoMove);
    const float availW = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, int(availW / gridSize_));
    const float cellW = std::floor(availW / cols);
    const float headH = ImGui::GetTextLineHeight() + 6, footH = 22.0f;
    const float thumbH = std::floor((cellW - 12) * 0.75f), cellH = headH + thumbH + footH + 8;
    const int rows = (int(shown.size()) + cols - 1) / cols;

    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0.0f)
        gridSize_ = std::clamp(gridSize_ * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f), 100.0f, 360.0f);

    // ---- keys: culling, arrows move through the grid (Shift extends), Enter / E open
    if (keys && !io.KeyAlt && !shown.empty()) {
        if (!io.KeyCtrl) cullKeys();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            for (int i : shown) items_[size_t(i)]->selected = true;
        } else if (!io.KeyCtrl) {
            int move = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -cols;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = cols;
            if (move) {
                const int p = std::clamp(posOf(focus_) + move, 0, int(shown.size()) - 1);
                focus_ = shown[size_t(p)];
                select(focus_, false, io.KeyShift);
                scrollToFocus_ = true;
            }
            if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_E, false)) && focus_ >= 0) {
                a.open = focus_;
                grid = false;
            }
        }
    }
    if (keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) grid = false;

    // Keep the focused card in view (after arrow keys, or on opening the grid).
    if (scrollToFocus_ && focus_ >= 0) {
        const float top = float(posOf(focus_) / cols) * cellH, winH = ImGui::GetWindowHeight();
        if (top < ImGui::GetScrollY()) ImGui::SetScrollY(top);
        else if (top + cellH > ImGui::GetScrollY() + winH) ImGui::SetScrollY(top + cellH - winH);
        scrollToFocus_ = false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(rows, cellH);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int col = 0; col < cols; ++col) {
                const size_t p = size_t(row) * cols + col;
                if (p >= shown.size()) break;
                const int i = shown[p];
                Item& it = *items_[size_t(i)];
                if (col) ImGui::SameLine(0, 0);
                ImGui::PushID(i);
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##card", ImVec2(cellW, cellH));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemVisible() && !it.requested) request(i, false);

                // The card, lighter when selected (Lightroom's grid).
                const ImVec2 c0(p0.x + 3, p0.y + 3), c1(p0.x + cellW - 3, p0.y + cellH - 3);
                const ImU32 card = it.selected ? IM_COL32(88, 88, 96, 255) : hovered ? IM_COL32(62, 62, 68, 255) : IM_COL32(48, 48, 53, 255);
                dl->AddRectFilled(c0, c1, card, 3.0f);
                if (i == current_) dl->AddRect(c0, c1, IM_COL32_WHITE, 3.0f, 0, 2.0f);
                else if (it.selected) dl->AddRect(c0, c1, kAccent, 3.0f, 0, 1.5f);
                if (i == focus_ && keys) dl->AddRect(ImVec2(c0.x + 2, c0.y + 2), ImVec2(c1.x - 2, c1.y - 2), IM_COL32(255, 255, 255, 60), 2.0f);

                // Header: its number in the folder and the file name.
                char num[16];
                std::snprintf(num, sizeof num, "%d", i + 1);
                dl->AddText(ImVec2(c0.x + 6, c0.y + 3), IM_COL32(150, 150, 158, 255), num);
                const std::string fname = pathToU8(u8ToPath(it.path).filename());
                const float numW = ImGui::CalcTextSize(num).x + 14;
                dl->PushClipRect(ImVec2(c0.x + numW, c0.y), ImVec2(c1.x - 4, c0.y + headH), true);
                dl->AddText(ImVec2(c0.x + numW, c0.y + 3), IM_COL32(200, 200, 206, 255), fname.c_str());
                dl->PopClipRect();

                const ImVec2 t0(c0.x + 6, p0.y + headH + 2), t1(c1.x - 6, p0.y + headH + 2 + thumbH);
                drawThumb(dl, it, t0, t1);

                // Footer: five stars to click (the given rating again clears it, as in Lightroom).
                const float starR = 6.0f, gap = 16.0f, fy = t1.y + footH * 0.5f + 2;
                const float sx0 = (c0.x + c1.x) * 0.5f - 2 * gap;
                int starHit = -1;
                if (hovered && std::fabs(io.MousePos.y - fy) < 9)
                    for (int s = 0; s < 5; ++s)
                        if (std::fabs(io.MousePos.x - (sx0 + s * gap)) < gap * 0.5f) starHit = s;
                for (int s = 0; s < 5; ++s) {
                    const bool on = starHit >= 0 ? s <= starHit : s < it.meta.rating;
                    const ImU32 col = starHit >= 0 ? (on ? IM_COL32(255, 220, 140, 255) : IM_COL32(110, 110, 118, 255))
                                                   : on ? IM_COL32(235, 235, 235, 255) : it.selected ? IM_COL32(150, 150, 158, 255) : IM_COL32(95, 95, 103, 255);
                    drawStar(dl, ImVec2(sx0 + s * gap, fy), starR, col, on);
                }

                if (ImGui::IsItemClicked()) {
                    if (starHit >= 0) {
                        const int r = it.meta.rating == starHit + 1 ? 0 : starHit + 1;
                        it.meta.rating = r;
                        writeMeta(i);
                        status = r ? "Rated " + std::to_string(r) + (r == 1 ? " star" : " stars") : "Rating cleared";
                    } else {
                        select(i, io.KeyCtrl, io.KeyShift);
                        focus_ = i;
                    }
                }
                if (hovered && starHit < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    a.open = i;
                    grid = false;
                }
                if (hovered && starHit < 0)
                    ImGui::SetTooltip("%s%s%s", fname.c_str(), library::hasSidecar(it.path) ? "\nEdit in " : "",
                                      library::hasSidecar(it.path) ? pathToU8(u8ToPath(library::sidecarPath(it.path)).filename()).c_str() : "");
                ImGui::PopID();
            }
        }
    }
    if (shown.empty()) ImGui::TextDisabled("No photos match the filter");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return a;
}
