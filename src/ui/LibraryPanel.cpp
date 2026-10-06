#include "ui/LibraryPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <tuple>

#include <imgui.h>

#include "graph/Graph.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "core/Parallel.h"

namespace {

// Lightroom's Library Filter, reduced to the attributes NodeLab keeps.
const char* const kFilters[] = {"All Photos", "Picked", "Hide Rejected", "Rejected",
                                "1 Star or More", "2 Stars or More", "3 Stars or More",
                                "4 Stars or More", "5 Stars", "Edited",
                                "Red Label", "Yellow Label", "Green Label", "Blue Label", "Purple Label",
                                "Duplicates"};
constexpr int kFirstLabelFilter = 10, kDuplicatesFilter = 15;

// Long edge of the large previews Compare and Survey show.
constexpr int kPreviewEdge = 1280;

ImU32 labelColor(int label) {
    static const ImU32 cols[] = {IM_COL32(0, 0, 0, 0), IM_COL32(220, 60, 60, 255), IM_COL32(230, 200, 60, 255),
                                 IM_COL32(90, 180, 80, 255), IM_COL32(70, 130, 230, 255), IM_COL32(160, 90, 200, 255)};
    return label > 0 && label < library::kLabelCount ? cols[label] : cols[0];
}

const char* const kSorts[] = {"File Name", "Capture Time", "File Type", "Rating", "Pick", "Edit Time",
                              "Camera", "Lens", "ISO", "Focal Length"};

const ImU32 kAccent = IM_COL32(90, 150, 255, 255);

// "IMG_1.jpg" or "IMG_1.jpg (Copy 2)".
std::string entryName(const std::string& path, int copy) {
    std::string n = pathToU8(u8ToPath(path).filename());
    if (copy > 0) n += " (Copy " + std::to_string(copy) + ")";
    return n;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// A file's modification time as seconds, or 0.
long long fileTime(const std::string& pathU8) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(u8ToPath(pathU8), ec);
    return ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

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
    dupStop_ = true;
    if (dupThread_.joinable()) dupThread_.join();
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool LibraryPanel::open(const std::string& dirU8) {
    std::vector<library::Entry> entries = library::listEntries(dirU8);
    if (entries.empty()) return false;
    dir_ = folder_ = dirU8;
    collection_.clear();
    loadEntries(std::move(entries));
    return true;
}

bool LibraryPanel::openCollection(const std::string& name) {
    collections_ = library::loadCollections();
    const auto c = std::find_if(collections_.begin(), collections_.end(), [&](const library::Collection& x) { return x.name == name; });
    if (c == collections_.end()) {
        status = "There is no collection " + name;
        return false;
    }
    // Photos moved or deleted since are left out (the collection keeps them).
    std::vector<library::Entry> entries;
    for (const library::Entry& e : c->entries) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(u8ToPath(e.photo), ec) && (e.copy == 0 || library::hasSidecar(e.photo, e.copy)))
            entries.push_back(e);
    }
    if (entries.empty()) {
        status = "The collection " + name + " has no photos that can be found";
        return false;
    }
    if (dir_.empty()) dir_ = folder_ = pathToU8(u8ToPath(entries[0].photo).parent_path());
    collection_ = name;
    loadEntries(std::move(entries));
    return true;
}

void LibraryPanel::loadEntries(std::vector<library::Entry> entries) {
    {
        std::lock_guard lock(mutex_);
        ++gen_;
        jobs_.clear();
        results_.clear();
    }
    items_.clear();
    expanded_.clear();
    for (library::Entry& e : entries) {
        auto it = std::make_unique<Item>();
        it->path = std::move(e.photo);
        it->copy = e.copy;
        library::readMeta(it->path, it->meta, it->copy);
        items_.push_back(std::move(it));
    }
    collections_ = library::loadCollections();
    current_ = anchor_ = focus_ = select_ = candidate_ = active_ = -1;
    if (filter_ == kDuplicatesFilter) filter_ = 0;
    if (sortBy != ByName || sortDescending) sort();
    else keepStacksTogether();
    if (!thread_.joinable()) thread_ = std::thread([this] { work(); });
}

void LibraryPanel::reorder(const std::vector<Item*>& order) {
    const std::array<int*, 6> idx = {&current_, &anchor_, &focus_, &select_, &candidate_, &active_};
    std::array<const Item*, 6> was{};
    for (size_t k = 0; k < idx.size(); ++k) was[k] = *idx[k] >= 0 && *idx[k] < size() ? items_[size_t(*idx[k])].get() : nullptr;
    std::map<const Item*, size_t> at;
    for (size_t k = 0; k < items_.size(); ++k) at[items_[k].get()] = k;
    std::vector<std::unique_ptr<Item>> next;
    for (Item* it : order) next.push_back(std::move(items_[at[it]]));
    items_ = std::move(next);
    for (size_t k = 0; k < idx.size(); ++k) {
        *idx[k] = -1;
        for (int i = 0; i < size() && was[k]; ++i)
            if (items_[size_t(i)].get() == was[k]) *idx[k] = i;
    }
}

void LibraryPanel::sort() {
    const bool needsInfo = sortBy == ByCaptureTime || sortBy == ByCamera || sortBy == ByLens || sortBy == ByIso ||
                           sortBy == ByFocalLength;
    if (needsInfo) {
        // Once per folder, on all cores: JPEGs and TIFF-based RAWs read a little of the file.
        std::vector<Item*> todo;
        for (auto& it : items_)
            if (!it->infoRead) todo.push_back(it.get());
        parallelFor(int(todo.size()), [&](int k) {
            exif::readInfo(todo[size_t(k)]->path, todo[size_t(k)]->info);
            todo[size_t(k)]->infoRead = true;
        });
    }
    // Sort keys computed once per entry (edit and file times read the disk).
    struct Key {
        std::string text;
        double number = 0;
    };
    std::vector<std::pair<Key, Item*>> keyed;
    for (auto& up : items_) {
        Item& it = *up;
        Key k;
        switch (sortBy) {
            case ByCaptureTime:
                // EXIF's "YYYY:MM:DD HH:MM:SS" sorts as text; without one, the file's time.
                k.text = it.info.captureTime;
                if (k.text.empty()) k.number = double(fileTime(it.path));
                break;
            case ByFileType: k.text = lowerAscii(pathToU8(u8ToPath(it.path).extension())); break;
            case ByRating: k.number = it.meta.rating; break;
            case ByPick: k.number = it.meta.flag; break;
            case ByEditTime: k.number = library::hasSidecar(it.path, it.copy) ? double(fileTime(library::sidecarPath(it.path, it.copy))) : 0; break;
            case ByCamera: k.text = lowerAscii(it.info.make + " " + it.info.model); break;
            case ByLens: k.text = lowerAscii(it.info.lens); break;
            case ByIso: k.number = it.info.iso; break;
            case ByFocalLength: k.number = it.info.focalLength; break;
            default: break;
        }
        keyed.push_back({std::move(k), &it});
    }
    const bool desc = sortDescending;
    std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
        // Entries without the value (no EXIF date) go last whichever the direction, so dated photos read in order.
        const bool aMissing = sortBy == ByCaptureTime && a.first.text.empty(), bMissing = sortBy == ByCaptureTime && b.first.text.empty();
        if (aMissing != bMissing) return bMissing;
        const auto ka = std::tie(a.first.text, a.first.number), kb = std::tie(b.first.text, b.first.number);
        if (ka != kb) return desc ? kb < ka : ka < kb;
        // Ties by name, then copy number, so a photo's virtual copies stay together.
        const std::string na = lowerAscii(pathToU8(u8ToPath(a.second->path).filename()));
        const std::string nb = lowerAscii(pathToU8(u8ToPath(b.second->path).filename()));
        if (na != nb) return sortBy == ByName && desc ? nb < na : na < nb;
        return a.second->copy < b.second->copy;
    });
    std::vector<Item*> order;
    for (auto& [k, it] : keyed) order.push_back(it);
    reorder(order);
    // A stack stays together, at its top photo's place.
    keepStacksTogether();
    scrollToCurrent_ = scrollToFocus_ = true;
}

int LibraryPanel::insertCopy(int i, int copy) {
    if (i < 0 || i >= size()) return -1;
    const std::string path = photo(i);
    // After the photo's last entry, so copies stay in number order.
    int at = i + 1;
    while (at < size() && items_[size_t(at)]->path == path) ++at;
    auto it = std::make_unique<Item>();
    it->path = path;
    it->copy = copy;
    it->info = items_[size_t(i)]->info;
    it->infoRead = items_[size_t(i)]->infoRead;
    library::readMeta(path, it->meta, copy);
    const Item* added = it.get();
    items_.insert(items_.begin() + at, std::move(it));
    for (int* idx : {&current_, &anchor_, &focus_, &select_, &candidate_, &active_})
        if (*idx >= at) ++*idx;
    // A copy of a stacked photo is in its stack (its sidecar was copied).
    keepStacksTogether();
    for (int i = 0; i < size(); ++i)
        if (items_[size_t(i)].get() == added) return i;
    return at;
}

void LibraryPanel::removeEntry(int i) {
    if (i < 0 || i >= size()) return;
    items_.erase(items_.begin() + i);
    for (int* idx : {&current_, &anchor_, &focus_, &select_, &candidate_, &active_})
        if (*idx == i) *idx = -1;
        else if (*idx > i) --*idx;
    updateStacks();
}

void LibraryPanel::setCurrentProject(const std::string& projectU8) {
    int found = -1;
    for (int i = 0; i < size(); ++i)
        if (sidecar(i) == projectU8) found = i;
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
        if (items_[size_t(i)]->selected && passes(i)) out.push_back(i);
    if (out.empty() && current_ >= 0) out.push_back(current_);
    return out;
}

void LibraryPanel::refresh(int i, bool rerender) {
    if (i < 0 || i >= size()) return;
    Item& it = *items_[size_t(i)];
    library::Meta m;
    if (library::readMeta(it.path, m, it.copy)) {
        if (rerender) m.thumb.clear();
        it.meta = m;
    }
    request(i, rerender);
    // The large preview showed the old edit.
    if (it.previewRequested) {
        it.previewRequested = false;
        if (rerender) it.preview.reset();
    }
}

int LibraryPanel::step(int from, int dir) const {
    // In the order shown (duplicates are grouped), from where `from` is.
    const std::vector<int> s = shown();
    const auto it = std::find(s.begin(), s.end(), from);
    if (it != s.end()) {
        const std::ptrdiff_t p = (it - s.begin()) + dir;
        return p >= 0 && p < std::ptrdiff_t(s.size()) ? s[size_t(p)] : -1;
    }
    // Not shown itself: the nearest one in list order.
    if (dir > 0) {
        for (int i : s)
            if (i > from) return i;
    } else {
        for (auto r = s.rbegin(); r != s.rend(); ++r)
            if (*r < from) return *r;
    }
    return -1;
}

bool LibraryPanel::passes(int i) const {
    const Item& it = *items_[size_t(i)];
    const library::Meta& m = it.meta;
    if (search_[0] && !library::matchesSearch(it.path, m, search_)) return false;
    // Duplicates show every member, stacked or not.
    if (filter_ == kDuplicatesFilter) return it.dupGroup > 0;
    if (const StackInfo* s = stackOf(i); s && s->top != i && !expanded_.count(m.stack)) return false;
    switch (filter_) {
        case 1: return m.flag == library::Picked;
        case 2: return m.flag != library::Rejected;
        case 3: return m.flag == library::Rejected;
        case 4: case 5: case 6: case 7: case 8: return m.rating >= filter_ - 3;
        case 9: return m.edited;
        default:
            if (filter_ >= kFirstLabelFilter && filter_ < kDuplicatesFilter) return m.label == filter_ - kFirstLabelFilter + 1;
            return true;
    }
}

std::vector<int> LibraryPanel::shown() const {
    std::vector<int> out;
    for (int i = 0; i < size(); ++i)
        if (passes(i)) out.push_back(i);
    if (filter_ == kDuplicatesFilter)
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return items_[size_t(a)]->dupGroup < items_[size_t(b)]->dupGroup; });
    return out;
}

void LibraryPanel::writeMeta(int i) {
    std::string err;
    if (!library::writeMeta(photo(i), meta(i), err, copyOf(i))) status = "Could not save the photo's metadata: " + err;
}

void LibraryPanel::setRating(const std::vector<int>& to, int rating) {
    for (int i : to) {
        meta(i).rating = rating;
        writeMeta(i);
    }
    if (!to.empty()) status = rating ? "Rated " + std::to_string(rating) + (rating == 1 ? " star" : " stars") : "Rating cleared";
}

void LibraryPanel::setFlag(const std::vector<int>& to, int flag) {
    for (int i : to) {
        meta(i).flag = flag;
        writeMeta(i);
    }
    if (!to.empty()) status = flag == library::Picked ? "Flagged as Pick" : flag == library::Rejected ? "Flagged as Rejected" : "Flag removed";
}

void LibraryPanel::setLabel(const std::vector<int>& to, int label) {
    for (int i : to) {
        meta(i).label = label;
        writeMeta(i);
    }
    if (!to.empty()) status = label ? std::string("Label ") + library::labelName(label) : "Label removed";
}

void LibraryPanel::request(int i, bool render, int edge) {
    Item& it = *items_[size_t(i)];
    (edge == library::kThumbEdge ? it.requested : it.previewRequested) = true;
    std::lock_guard lock(mutex_);
    // Newest requests first: what scrolled into view, or the edit just saved.
    jobs_.push_front(Job{i, gen_, it.path, it.copy, it.meta, render, edge});
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
        Result r{job.index, job.gen, job.path, job.copy, nullptr, {}, job.edge != library::kThumbEdge};
        std::string err;
        if (r.preview) {
            // A large preview: the edit rendered at that size, or the photo itself.
            if (job.meta.edited && library::hasSidecar(job.path, job.copy)) {
                Graph g;
                nlohmann::json ui;
                if (loadProject(library::sidecarPath(job.path, job.copy), g, ui, err)) r.image = library::renderThumbnail(g, job.edge, err);
            }
            if (!r.image) r.image = library::loadThumbnail(job.path, job.edge, err);
        } else if (job.render || (job.meta.edited && job.meta.thumb.empty() && library::hasSidecar(job.path, job.copy))) {
            // An edited photo shows its edit: stored in the sidecar, or rendered (and then stored).
            Graph g;
            nlohmann::json ui;
            if (loadProject(library::sidecarPath(job.path, job.copy), g, ui, err))
                if ((r.image = library::renderThumbnail(g, library::kThumbEdge, err))) r.store = library::encodeThumb(*r.image);
        } else if (!job.meta.thumb.empty()) {
            r.image = library::decodeThumb(job.meta.thumb);
        }
        if (!r.image && !r.preview) r.image = library::loadThumbnail(job.path, library::kThumbEdge, err);
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
        if (!r.image) continue;
        // The list may have been sorted, or an entry added, since the job was queued.
        auto matches = [&](int i) { return i >= 0 && i < size() && items_[size_t(i)]->path == r.path && items_[size_t(i)]->copy == r.copy; };
        if (!matches(r.index)) {
            r.index = -1;
            for (int i = 0; i < size() && r.index < 0; ++i)
                if (matches(i)) r.index = i;
            if (r.index < 0) continue;
        }
        Item& it = *items_[size_t(r.index)];
        if (r.preview) {
            if (it.previewRequested) it.preview.upload(*r.image);  // unless released meanwhile
            continue;
        }
        it.tex.upload(*r.image);
        if (!r.store.empty()) {
            it.meta.thumb = std::move(r.store);
            writeMeta(r.index);
        }
    }

    // Find Duplicates finished: its groups, by path (the list may have been sorted since).
    DupResult dr;
    {
        std::lock_guard lock(mutex_);
        if (dupResult_.ready) std::swap(dr, dupResult_);
    }
    if (!dr.ready) return;
    if (dupThread_.joinable()) dupThread_.join();
    if (dr.gen != gen_) return;
    for (auto& it : items_) it->dupGroup = 0;
    int photos = 0;
    for (size_t g = 0; g < dr.groups.size(); ++g)
        for (int k : dr.groups[g])
            for (auto& it : items_)
                if (it->copy == 0 && it->path == dr.paths[size_t(k)]) it->dupGroup = int(g) + 1, ++photos;
    if (dr.groups.empty()) {
        status = "No duplicates found";
        return;
    }
    filter_ = kDuplicatesFilter;
    scrollToCurrent_ = scrollToFocus_ = true;
    status = "Found " + std::to_string(dr.groups.size()) + (dr.groups.size() == 1 ? " group" : " groups") + " of duplicates (" +
             std::to_string(photos) + " photos); the filter shows them";
}

void LibraryPanel::contextMenu(int i, Actions& a) {
    if (!ImGui::BeginPopupContextItem("##entryMenu")) return;
    // As in Lightroom, a right-click acts on the selection when the photo is part of it.
    if (ImGui::IsWindowAppearing() && !items_[size_t(i)]->selected) select(i, false, false);
    if (ImGui::MenuItem("Create Virtual Copy", "Ctrl+'")) a.createCopy = i;
    if (copyOf(i) > 0 && ImGui::MenuItem("Remove Virtual Copy...")) a.removeCopy = i;
    ImGui::Separator();
    if (ImGui::MenuItem("Group into Stack", "Ctrl+G")) groupStack();
    if (stackOf(i)) {
        if (ImGui::MenuItem(expanded_.count(meta(i).stack) ? "Collapse Stack" : "Expand Stack", "S")) toggleStack(i);
        if (ImGui::MenuItem("Unstack", "Ctrl+Shift+G")) unstack();
    }
    if (ImGui::BeginMenu("Color Label")) {
        for (int l = 0; l < library::kLabelCount; ++l) {
            const char* keys[] = {"", "6", "7", "8", "9", ""};
            if (ImGui::MenuItem(library::labelName(l), keys[l], meta(i).label == l)) setLabel(selection(), l);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add to Collection")) {
        for (const library::Collection& c : collections_)
            if (c.name != collection_ && ImGui::MenuItem(c.name.c_str())) addSelectionTo(c.name);
        if (!collections_.empty()) ImGui::Separator();
        ImGui::SetNextItemWidth(180);
        if (ImGui::InputTextWithHint("##newCollectionCtx", "New collection", newCollection_, sizeof newCollection_,
                                     ImGuiInputTextFlags_EnterReturnsTrue) && newCollection_[0]) {
            addSelectionTo(newCollection_);
            newCollection_[0] = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    // Copy Edit copies the photo being edited.
    if (ImGui::MenuItem("Copy Edit", "Ctrl+Shift+C", false, i == current_)) a.copy = true;
    if (ImGui::MenuItem("Paste Edit", "Ctrl+Shift+V", false, canPaste)) a.paste = true;
    if (ImGui::MenuItem("Export Selected...")) a.exportSelected = true;
    ImGui::EndPopup();
}

void LibraryPanel::cullKeys(const std::vector<int>& to) {
    // Lightroom's culling keys: 0-5 rate, P pick, X reject, U unflag, 6-9 colour labels (the
    // label they already have clears it).
    for (int r = 0; r <= 5; ++r)
        if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_0 + r), false) || ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad0 + r), false))
            setRating(to, r);
    for (int l = 1; l <= 4; ++l)
        if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_5 + l), false) || ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad5 + l), false)) {
            const bool all = !to.empty() && std::all_of(to.begin(), to.end(), [&](int i) { return meta(i).label == l; });
            setLabel(to, all ? library::NoLabel : l);
        }
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) setFlag(to, library::Picked);
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) setFlag(to, library::Rejected);
    if (ImGui::IsKeyPressed(ImGuiKey_U, false)) setFlag(to, library::Unflagged);
}

void LibraryPanel::select(int i, bool ctrl, bool shift) {
    if (ctrl) {
        items_[size_t(i)]->selected = !items_[size_t(i)]->selected;
        anchor_ = i;
    } else if (shift && anchor_ >= 0) {
        const int lo = std::min(anchor_, i), hi = std::max(anchor_, i);
        for (int k = 0; k < size(); ++k) items_[size_t(k)]->selected = k >= lo && k <= hi && passes(k);
    } else {
        for (int k = 0; k < size(); ++k) items_[size_t(k)]->selected = k == i;
        anchor_ = i;
    }
}

LibraryPanel::Actions LibraryPanel::draw(bool keys) {
    Actions a;
    ImGuiIO& io = ImGui::GetIO();

    // ---- culling keys, and the arrows step through the photos
    if (previewsShown_) releasePreviews();
    if (keys && !io.KeyCtrl && !io.KeyAlt) {
        cullKeys(selection());
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) a.open = step(current_, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) a.open = step(current_, +1);
        if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyShift) toggleStack(current_);
    }
    if (keys && io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_G, false)) io.KeyShift ? unstack() : groupStack();

    toolbar(a);
    ImGui::SameLine();
    viewButtons();

    ImGui::BeginChild("##strip", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    return drawStrip(a);
}

void LibraryPanel::toolbar(Actions& a) {
    const std::string folderName = pathToU8(u8ToPath(dir_).filename());
    const std::string name = collection_.empty() ? folderName : "Collection: " + collection_;
    const int shown = int(this->shown().size());
    ImGui::TextDisabled("%s", name.empty() ? dir_.c_str() : name.c_str());
    ImGui::SameLine();
    if (current_ >= 0)
        ImGui::TextDisabled("%d / %d   %s", current_ + 1, size(), entryName(photo(current_), copyOf(current_)).c_str());
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
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("##sort", &sortBy, kSorts, IM_ARRAYSIZE(kSorts))) sort();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sort by");
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton(sortDescending ? "Z-A##sortDir" : "A-Z##sortDir")) {
        sortDescending = !sortDescending;
        sort();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(sortDescending ? "Descending (click for ascending)" : "Ascending (click for descending)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputTextWithHint("##search", "Search", search_, sizeof search_)) scrollToCurrent_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show photos whose file name, title, caption or keywords contain every word");
    ImGui::SameLine();
    collectionsMenu(a);
    ImGui::SameLine();
    photoMenu();
    ImGui::SameLine();
    if (ImGui::SmallButton(showMetadata ? "Metadata <" : "Metadata >")) showMetadata = !showMetadata;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Title, caption, keywords and colour label of the selected photos");
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
    if (dupRunning_) {
        ImGui::SameLine();
        ImGui::TextDisabled("Finding duplicates %d / %d", dupDone_.load(), dupTotal_.load());
    }
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
    for (int i : shown()) {
        Item& it = *items_[size_t(i)];
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
        contextMenu(i, a);
        if (i == current_ && scrollToCurrent_) {
            ImGui::SetScrollHereX(0.5f);
            scrollToCurrent_ = false;
        }
        if (ImGui::IsItemVisible() && !it.requested) request(i, false);

        const ImVec2 b0(p0.x + 4, p0.y + 2), b1(p0.x + 4 + thumbW, p0.y + thumbH - 2);
        drawThumb(dl, i, b0, b1);
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
            const bool side = library::hasSidecar(it.path, it.copy);
            ImGui::SetTooltip("%s%s%s", entryName(it.path, it.copy).c_str(), side ? "\nEdit in " : "",
                              side ? pathToU8(u8ToPath(library::sidecarPath(it.path, it.copy)).filename()).c_str() : "");
        }
        ImGui::PopID();
    }
    if (first) ImGui::TextDisabled("No photos match the filter");
    ImGui::EndChild();
    return a;
}

void LibraryPanel::drawThumb(ImDrawList* dl, int i, ImVec2 b0, ImVec2 b1) const {
    const Item& it = *items_[size_t(i)];
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
    if (it.copy > 0) {
        // Lightroom marks virtual copies with a turned-up page corner at the bottom left.
        const ImVec2 c(b0.x, b1.y);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - 16), ImVec2(c.x + 16, c.y), c, IM_COL32(0, 0, 0, 160));
        dl->AddTriangleFilled(ImVec2(c.x, c.y - 13), ImVec2(c.x + 13, c.y), ImVec2(c.x + 13, c.y - 13), IM_COL32(235, 235, 235, 255));
    }
    if (it.meta.edited) {
        // Outlined, so it shows on a blue sky too.
        dl->AddTriangleFilled(ImVec2(b1.x - 17, b0.y), ImVec2(b1.x, b0.y), ImVec2(b1.x, b0.y + 17), IM_COL32(0, 0, 0, 160));
        dl->AddTriangleFilled(ImVec2(b1.x - 14, b0.y), ImVec2(b1.x, b0.y), ImVec2(b1.x, b0.y + 14), kAccent);
    }
    // Colour label: a bar along the bottom.
    if (it.meta.label != library::NoLabel) dl->AddRectFilled(ImVec2(b0.x, b1.y - 4), b1, labelColor(it.meta.label));
    // Stack: the count on a card with another behind it (Lightroom's badge), at the bottom right;
    // the other members show their place in it.
    if (const StackInfo* s = stackOf(i)) {
        int place = 1;
        for (int k = s->top; k < i; ++k) place += items_[size_t(k)]->meta.stack == it.meta.stack;
        char text[24];
        if (place == 1) std::snprintf(text, sizeof text, "%d", s->count);
        else std::snprintf(text, sizeof text, "%d/%d", place, s->count);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 r1(b1.x - 5, b1.y - 8), r0(r1.x - ts.x - 10, r1.y - ts.y - 4);
        if (place == 1) dl->AddRectFilled(ImVec2(r0.x + 3, r0.y - 3), ImVec2(r1.x + 3, r1.y - 3), IM_COL32(150, 150, 158, 230), 2.0f);
        dl->AddRectFilled(r0, r1, IM_COL32(20, 20, 24, 235), 2.0f);
        dl->AddRect(r0, r1, IM_COL32(210, 210, 216, 255), 2.0f);
        dl->AddText(ImVec2(r0.x + 5, r0.y + 2), IM_COL32_WHITE, text);
    }
    // Find Duplicates' group, at the top in the middle.
    if (it.dupGroup > 0 && filter_ == kDuplicatesFilter) {
        char text[24];
        std::snprintf(text, sizeof text, "Group %d", it.dupGroup);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 r0((b0.x + b1.x - ts.x) * 0.5f - 5, b0.y + 4), r1(r0.x + ts.x + 10, r0.y + ts.y + 4);
        dl->AddRectFilled(r0, r1, IM_COL32(200, 120, 40, 235), 2.0f);
        dl->AddText(ImVec2(r0.x + 5, r0.y + 2), IM_COL32_WHITE, text);
    }
}

LibraryPanel::Actions LibraryPanel::drawGrid(bool keys) {
    if (view == CompareView) return drawCompare(keys);
    if (view == SurveyView) return drawSurvey(keys);
    if (previewsShown_) releasePreviews();
    Actions a;
    ImGuiIO& io = ImGui::GetIO();
    // The cells: the photos the filter shows, in order.
    const std::vector<int> shown = this->shown();
    auto posOf = [&](int i) { return int(std::find(shown.begin(), shown.end(), i) - shown.begin()); };
    if (focus_ < 0 || focus_ >= size() || !passes(focus_))
        focus_ = current_ >= 0 && passes(current_) ? current_ : shown.empty() ? -1 : shown[0];

    toolbar(a);
    // The views and their own controls on a second row (the toolbar fills the first).
    viewButtons();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("##size", &gridSize_, 100.0f, 360.0f, "Size %.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thumbnail size (Ctrl+wheel)");

    const bool side = beginMain();
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
    if (keys && !io.KeyAlt && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) io.KeyShift ? unstack() : groupStack();
    if (keys && !io.KeyAlt && !io.KeyCtrl && !io.KeyShift) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) toggleStack(focus_);
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) view = CompareView, select_ = candidate_ = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) view = SurveyView;
    }
    if (keys && !io.KeyAlt && !shown.empty()) {
        if (!io.KeyCtrl) cullKeys(selection());
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
                contextMenu(i, a);
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
                const std::string fname = entryName(it.path, it.copy);
                const float numW = ImGui::CalcTextSize(num).x + 14;
                dl->PushClipRect(ImVec2(c0.x + numW, c0.y), ImVec2(c1.x - 4, c0.y + headH), true);
                dl->AddText(ImVec2(c0.x + numW, c0.y + 3), IM_COL32(200, 200, 206, 255), fname.c_str());
                dl->PopClipRect();

                const ImVec2 t0(c0.x + 6, p0.y + headH + 2), t1(c1.x - 6, p0.y + headH + 2 + thumbH);
                drawThumb(dl, i, t0, t1);

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
                    ImGui::SetTooltip("%s%s%s", fname.c_str(), library::hasSidecar(it.path, it.copy) ? "\nEdit in " : "",
                                      library::hasSidecar(it.path, it.copy)
                                          ? pathToU8(u8ToPath(library::sidecarPath(it.path, it.copy)).filename()).c_str()
                                          : "");
                ImGui::PopID();
            }
        }
    }
    if (shown.empty()) ImGui::TextDisabled("No photos match the filter");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    endMain(side);
    return a;
}

// ---------------------------------------------------------------- stacks

void LibraryPanel::updateStacks() {
    stacks_.clear();
    for (int i = 0; i < size(); ++i) {
        const std::string& id = items_[size_t(i)]->meta.stack;
        if (id.empty()) continue;
        StackInfo& s = stacks_[id];
        if (s.top < 0) s.top = i;
        ++s.count;
    }
    // A stack left with one photo (the others removed or in another folder) isn't one.
    for (auto it = stacks_.begin(); it != stacks_.end();) it = it->second.count < 2 ? stacks_.erase(it) : std::next(it);
    for (auto it = expanded_.begin(); it != expanded_.end();) it = stacks_.count(*it) ? std::next(it) : expanded_.erase(it);
}

void LibraryPanel::keepStacksTogether() {
    updateStacks();
    if (stacks_.empty()) return;
    std::vector<Item*> order;
    std::set<std::string> placed;
    for (int i = 0; i < size(); ++i) {
        const std::string& id = items_[size_t(i)]->meta.stack;
        if (!stacks_.count(id)) {
            order.push_back(items_[size_t(i)].get());
            continue;
        }
        if (!placed.insert(id).second) continue;
        for (int k = i; k < size(); ++k)
            if (items_[size_t(k)]->meta.stack == id) order.push_back(items_[size_t(k)].get());
    }
    reorder(order);
    updateStacks();
}

const LibraryPanel::StackInfo* LibraryPanel::stackOf(int i) const {
    if (i < 0 || i >= size()) return nullptr;
    const auto it = stacks_.find(items_[size_t(i)]->meta.stack);
    return it == stacks_.end() ? nullptr : &it->second;
}

std::vector<int> LibraryPanel::selectionWithStacks() const {
    const std::vector<int> sel = selection();
    std::set<int> in(sel.begin(), sel.end());
    for (int i : sel)
        if (const StackInfo* s = stackOf(i); s && s->top == i && !expanded_.count(meta(i).stack))
            for (int k = 0; k < size(); ++k)
                if (meta(k).stack == meta(i).stack) in.insert(k);
    return {in.begin(), in.end()};
}

void LibraryPanel::groupStack() {
    const std::vector<int> sel = selectionWithStacks();
    if (sel.size() < 2) {
        status = "Select two or more photos to stack them";
        return;
    }
    // Named after its top photo, plus a hash of its full path and the time: a collection lists
    // photos from several folders, and two folders' stacks topped by "IMG_0001.jpg" mustn't merge.
    const std::string base = entryName(photo(sel[0]), copyOf(sel[0]));
    auto taken = [&](const std::string& id) {
        for (int i = 0; i < size(); ++i)
            if (meta(i).stack == id && !std::binary_search(sel.begin(), sel.end(), i)) return true;
        return false;
    };
    const uint64_t salt = std::hash<std::string>{}(photo(sel[0]) + "|" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    char tag[12];
    std::snprintf(tag, sizeof tag, "#%08x", unsigned(salt ^ (salt >> 32)));
    std::string id = base + tag;
    for (int n = 2; taken(id); ++n) id = base + tag + "-" + std::to_string(n);
    for (int i : sel) {
        meta(i).stack = id;
        writeMeta(i);
    }
    expanded_.erase(id);
    keepStacksTogether();
    // The collapsed stack shows its top photo: keep the keyboard there.
    if (const auto s = stacks_.find(id); s != stacks_.end()) focus_ = s->second.top;
    scrollToCurrent_ = scrollToFocus_ = true;
    status = "Stacked " + std::to_string(sel.size()) + " photos";
}

void LibraryPanel::unstack() {
    std::set<std::string> ids;
    for (int i : selection())
        if (stackOf(i)) ids.insert(meta(i).stack);
    if (ids.empty()) return;
    int n = 0;
    for (int i = 0; i < size(); ++i)
        if (ids.count(meta(i).stack)) {
            meta(i).stack.clear();
            writeMeta(i);
            ++n;
        }
    updateStacks();
    status = "Unstacked " + std::to_string(n) + " photos";
}

void LibraryPanel::toggleStack(int i) {
    const StackInfo* s = stackOf(i);
    if (!s) return;
    const std::string id = meta(i).stack;
    if (!expanded_.erase(id)) {
        expanded_.insert(id);
    } else {
        // Collapsed: the hidden members can't keep the keyboard or the selection.
        for (int k = 0; k < size(); ++k)
            if (k != s->top && meta(k).stack == id) items_[size_t(k)]->selected = false;
        if (focus_ >= 0 && meta(focus_).stack == id) focus_ = s->top;
    }
    scrollToCurrent_ = scrollToFocus_ = true;
}

// ---------------------------------------------------------------- collections and duplicates

void LibraryPanel::addSelectionTo(const std::string& name) {
    std::vector<library::Entry> entries;
    for (int i : selectionWithStacks()) entries.push_back({photo(i), copyOf(i)});
    if (entries.empty() || name.empty()) return;
    collections_ = library::loadCollections();
    const int added = library::addToCollection(collections_, name, entries);
    std::string err;
    if (!library::saveCollections(collections_, err)) {
        status = "Could not save the collections: " + err;
        return;
    }
    status = added ? "Added " + std::to_string(added) + (added == 1 ? " photo to " : " photos to ") + name
                   : "The photos are in " + name + " already";
}

void LibraryPanel::findDuplicates() {
    if (dupRunning_) return;
    if (dupThread_.joinable()) dupThread_.join();
    // Virtual copies share their photo's file, so only the photos are compared.
    std::vector<std::string> paths;
    for (const auto& it : items_)
        if (it->copy == 0) paths.push_back(it->path);
    if (paths.size() < 2) {
        status = "Find Duplicates needs two or more photos";
        return;
    }
    dupStop_ = false;
    dupDone_ = 0;
    dupTotal_ = int(paths.size());
    dupRunning_ = true;
    const uint64_t gen = gen_;
    dupThread_ = std::thread([this, paths = std::move(paths), gen] {
        const int n = int(paths.size());
        std::vector<uint64_t> files(static_cast<size_t>(n)), pictures(static_cast<size_t>(n));
        std::vector<float> aspects(static_cast<size_t>(n));
        std::vector<std::array<uint64_t, 3>> rotated(static_cast<size_t>(n));
        parallelFor(n, [&](int k) {
            if (dupStop_) return;
            files[size_t(k)] = library::fileHash(paths[size_t(k)]);
            // A small picture is enough for a 9 x 8 hash (a RAW's embedded preview).
            std::string err;
            if (ImagePtr t = library::loadThumbnail(paths[size_t(k)], 64, err); t && t->w > 0 && t->h > 0) {
                pictures[size_t(k)] = library::pictureHash(*t);
                rotated[size_t(k)] = library::rotatedHashes(*t);
                aspects[size_t(k)] = float(t->w) / float(t->h);
            }
            ++dupDone_;
        });
        DupResult r;
        r.gen = gen;
        r.paths = paths;
        if (!dupStop_) r.groups = library::groupDuplicates(files, pictures, aspects, 4, rotated);
        r.ready = true;
        std::lock_guard lock(mutex_);
        dupResult_ = std::move(r);
        dupRunning_ = false;
    });
    status = "Finding duplicates...";
}

void LibraryPanel::collectionsMenu(Actions& a) {
    if (ImGui::SmallButton("Collections")) {
        collections_ = library::loadCollections();
        ImGui::OpenPopup("##collections");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lightroom's collections: lists of photos from any folders");
    if (!ImGui::BeginPopup("##collections")) return;
    if (!collection_.empty() && !folder_.empty()) {
        const std::string back = "Back to Folder " + pathToU8(u8ToPath(folder_).filename());
        if (ImGui::MenuItem(back.c_str())) a.openFolder = folder_;
        ImGui::Separator();
    }
    if (collections_.empty()) ImGui::TextDisabled("No collections yet: name one below to add the selected photos");
    for (const library::Collection& c : collections_) {
        const std::string label = c.name + "  (" + std::to_string(c.entries.size()) + ")";
        if (ImGui::MenuItem(label.c_str(), nullptr, c.name == collection_)) a.openCollection = c.name;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Selected photos");
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputTextWithHint("##newCollection", "New collection (Enter)", newCollection_, sizeof newCollection_,
                                 ImGuiInputTextFlags_EnterReturnsTrue) && newCollection_[0]) {
        addSelectionTo(newCollection_);
        newCollection_[0] = 0;
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::BeginMenu("Add to Collection", !collections_.empty())) {
        for (const library::Collection& c : collections_)
            if (ImGui::MenuItem(c.name.c_str(), nullptr, false, c.name != collection_)) addSelectionTo(c.name);
        ImGui::EndMenu();
    }
    if (!collection_.empty()) {
        if (ImGui::MenuItem("Remove Selected from This Collection")) {
            const std::vector<int> sel = selectionWithStacks();
            auto c = std::find_if(collections_.begin(), collections_.end(), [&](const library::Collection& x) { return x.name == collection_; });
            if (c != collections_.end() && !sel.empty()) {
                std::erase_if(c->entries, [&](const library::Entry& e) {
                    return std::any_of(sel.begin(), sel.end(), [&](int i) { return e.copy == copyOf(i) && e.photo == photo(i); });
                });
                std::string err;
                if (library::saveCollections(collections_, err)) {
                    for (auto r = sel.rbegin(); r != sel.rend(); ++r) removeEntry(*r);
                    status = "Removed " + std::to_string(sel.size()) + " photos from " + collection_ + " (the files stay)";
                } else {
                    status = "Could not save the collections: " + err;
                }
            }
        }
        if (ImGui::BeginMenu("Delete This Collection")) {
            // A submenu, so a stray click doesn't delete it; the photos themselves stay.
            if (ImGui::MenuItem("Delete (the photos stay)")) {
                std::erase_if(collections_, [&](const library::Collection& x) { return x.name == collection_; });
                std::string err;
                if (library::saveCollections(collections_, err)) {
                    status = "Deleted the collection " + collection_;
                    a.openFolder = folder_;
                } else {
                    status = "Could not save the collections: " + err;
                }
            }
            ImGui::EndMenu();
        }
    }
    ImGui::EndPopup();
}

void LibraryPanel::photoMenu() {
    if (ImGui::SmallButton("Photo")) ImGui::OpenPopup("##photoMenu");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stacks, colour labels and Find Duplicates");
    if (!ImGui::BeginPopup("##photoMenu")) return;
    const std::vector<int> sel = selection();
    const bool anyStack = std::any_of(sel.begin(), sel.end(), [&](int i) { return stackOf(i) != nullptr; });
    if (ImGui::MenuItem("Group into Stack", "Ctrl+G", false, sel.size() >= 2)) groupStack();
    if (ImGui::MenuItem("Unstack", "Ctrl+Shift+G", false, anyStack)) unstack();
    if (ImGui::MenuItem("Expand or Collapse Stack", "S", false, anyStack)) toggleStack(sel[0]);
    if (ImGui::MenuItem("Expand All Stacks", nullptr, false, !stacks_.empty()))
        for (const auto& [id, s] : stacks_) expanded_.insert(id);
    if (ImGui::MenuItem("Collapse All Stacks", nullptr, false, !expanded_.empty())) {
        for (const std::string& id : std::set<std::string>(expanded_))
            if (const auto s = stacks_.find(id); s != stacks_.end()) toggleStack(s->second.top);
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Color Label", !sel.empty())) {
        const char* keys[] = {"", "6", "7", "8", "9", ""};
        for (int l = 0; l < library::kLabelCount; ++l)
            if (ImGui::MenuItem(library::labelName(l), keys[l], meta(sel[0]).label == l)) setLabel(sel, l);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Find Duplicates", nullptr, false, !dupRunning_)) findDuplicates();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Group the photos that are the same file, or the same picture re-saved or resized;\nthe Duplicates filter shows the groups");
    const bool anyDup = std::any_of(items_.begin(), items_.end(), [](const auto& it) { return it->dupGroup > 0; });
    if (ImGui::MenuItem("Clear Duplicate Groups", nullptr, false, anyDup)) {
        for (auto& it : items_) it->dupGroup = 0;
        if (filter_ == kDuplicatesFilter) filter_ = 0;
    }
    ImGui::Separator();
    ImGui::MenuItem("Metadata Panel", nullptr, &showMetadata);
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- Compare and Survey

void LibraryPanel::viewButtons() {
    auto button = [](const char* label, bool on, const char* tip) {
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked = ImGui::SmallButton(label);
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return clicked;
    };
    if (button("Grid", grid && view == GridView, "Show the whole folder as a grid (G)")) {
        grid = true;
        view = GridView;
        focus_ = current_;
        scrollToFocus_ = true;
    }
    ImGui::SameLine(0, 2);
    if (button("Compare", grid && view == CompareView, "Two photos side by side, zoomed together (C)")) {
        grid = true;
        view = CompareView;
        select_ = candidate_ = -1;
    }
    ImGui::SameLine(0, 2);
    if (button("Survey", grid && view == SurveyView, "The selected photos side by side (N)")) {
        grid = true;
        view = SurveyView;
    }
    ImGui::SameLine(0, 2);
    if (button("Loupe", !grid, "Back to the editor (Escape; E or a double-click opens a photo)")) grid = false;
}

void LibraryPanel::releasePreviews(const std::vector<int>& keep) {
    bool any = false;
    for (int i = 0; i < size(); ++i) {
        Item& it = *items_[size_t(i)];
        if (std::find(keep.begin(), keep.end(), i) != keep.end()) {
            any = any || it.previewRequested;
            continue;
        }
        if (it.previewRequested || it.preview.valid()) {
            it.preview.reset();
            it.previewRequested = false;
        }
    }
    previewsShown_ = any;
    // Previews not started yet aren't wanted any more (thumbnails still are).
    std::lock_guard lock(mutex_);
    std::erase_if(jobs_, [&](const Job& j) {
        return j.edge != library::kThumbEdge && std::find(keep.begin(), keep.end(), j.index) == keep.end();
    });
}

void LibraryPanel::drawPreview(ImDrawList* dl, int i, ImVec2 b0, ImVec2 b1, bool zoomed) {
    Item& it = *items_[size_t(i)];
    if (!it.requested) request(i, false);
    if (!it.previewRequested) request(i, false, kPreviewEdge);
    previewsShown_ = true;
    dl->AddRectFilled(b0, b1, IM_COL32(24, 24, 27, 255));
    // The thumbnail stands in until the preview arrives.
    const GLTexture& t = it.preview.valid() ? it.preview : it.tex;
    if (!t.valid()) return;
    const float bw = b1.x - b0.x, bh = b1.y - b0.y;
    const float fit = std::min(bw / t.width(), bh / t.height()), z = zoomed ? zoom_ : 1.0f;
    const float w = t.width() * fit * z, h = t.height() * fit * z;
    // The shared centre (0..1 of the picture) at the pane's centre, kept inside the picture.
    auto centre = [](float c, float view, float size) {
        if (size <= view) return 0.5f;
        const float half = 0.5f * view / size;
        return std::clamp(c, half, 1.0f - half);
    };
    const float cx = zoomed ? centre(centerX_, bw, w) : 0.5f, cy = zoomed ? centre(centerY_, bh, h) : 0.5f;
    const ImVec2 i0(b0.x + bw * 0.5f - cx * w, b0.y + bh * 0.5f - cy * h);
    const ImU32 tint = it.meta.flag == library::Rejected ? IM_COL32(255, 255, 255, 110) : IM_COL32_WHITE;
    dl->PushClipRect(b0, b1, true);
    dl->AddImage(ImTextureID(intptr_t(t.id())), i0, ImVec2(i0.x + w, i0.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
    dl->PopClipRect();
}

float LibraryPanel::captionHeight() { return ImGui::GetTextLineHeight() + 24.0f; }

void LibraryPanel::drawCaption(ImDrawList* dl, int i, ImVec2 p0, float w) const {
    const Item& it = *items_[size_t(i)];
    // Line 1: label swatch, flag and file name; line 2: the rating as stars.
    float x = p0.x + 4;
    const float lineH = ImGui::GetTextLineHeight();
    if (it.meta.label != library::NoLabel) {
        dl->AddRectFilled(ImVec2(x, p0.y + 2), ImVec2(x + 10, p0.y + lineH - 2), labelColor(it.meta.label), 2.0f);
        x += 14;
    }
    if (it.meta.flag != library::Unflagged) {
        const bool pick = it.meta.flag == library::Picked;
        dl->AddText(ImVec2(x, p0.y), pick ? IM_COL32_WHITE : IM_COL32(235, 70, 60, 255), pick ? "Pick" : "Rejected");
        x += ImGui::CalcTextSize(pick ? "Pick" : "Rejected").x + 8;
    }
    const std::string name = entryName(it.path, it.copy);
    dl->PushClipRect(ImVec2(x, p0.y), ImVec2(p0.x + w, p0.y + lineH + 2), true);
    dl->AddText(ImVec2(x, p0.y), IM_COL32(210, 210, 216, 255), name.c_str());
    dl->PopClipRect();
    for (int s = 0; s < 5; ++s)
        drawStar(dl, ImVec2(p0.x + 10 + s * 15.0f, p0.y + lineH + 10), 5.5f,
                 s < it.meta.rating ? IM_COL32(235, 235, 235, 255) : IM_COL32(95, 95, 103, 255), s < it.meta.rating);
}

LibraryPanel::Actions LibraryPanel::drawCompare(bool keys) {
    Actions a;
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<int> shown = this->shown();
    auto isShown = [&](int i) { return i >= 0 && std::find(shown.begin(), shown.end(), i) != shown.end(); };
    auto other = [&](int from) {
        const int n = step(from, +1);
        return n >= 0 ? n : step(from, -1);
    };
    // Lightroom: the Select is the first selected photo, the Candidate the second selected one
    // (else the photo after the Select).
    if (!isShown(select_)) {
        const std::vector<int> sel = selection();
        select_ = !sel.empty() && isShown(sel[0]) ? sel[0] : shown.empty() ? -1 : shown[0];
        candidate_ = sel.size() > 1 ? sel[1] : -1;
    }
    if (!isShown(candidate_) || candidate_ == select_) candidate_ = select_ >= 0 ? other(select_) : -1;
    if (active_ != select_ && active_ != candidate_) active_ = select_;
    releasePreviews({select_, candidate_});

    toolbar(a);
    viewButtons();
    ImGui::SameLine();
    ImGui::BeginDisabled(candidate_ < 0);
    if (ImGui::SmallButton("Swap")) std::swap(select_, candidate_);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Swap the Select and the Candidate (Down)");
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("Make Select")) {
        select_ = candidate_;
        candidate_ = other(select_);
        active_ = select_;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The Candidate becomes the Select, and the next photo the Candidate (Up)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::SliderFloat("##zoom", &zoom_, 1.0f, 8.0f, "Zoom %.1fx", ImGuiSliderFlags_Logarithmic) && zoom_ <= 1.0f)
        centerX_ = centerY_ = 0.5f;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Both photos zoom and pan together (wheel, and drag)");
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("Fit")) zoom_ = 1.0f, centerX_ = centerY_ = 0.5f;

    // ---- keys: culling on the active photo; Left / Right pick the Candidate
    if (keys && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        if (active_ >= 0) cullKeys({active_});
        for (int dir : {-1, +1})
            if (candidate_ >= 0 && ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow)) {
                int c = step(candidate_, dir);
                if (c == select_) c = step(c, dir);
                if (c >= 0) candidate_ = active_ = c;
            }
        if (candidate_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            select_ = candidate_;
            candidate_ = other(select_);
            active_ = select_;
        }
        if (candidate_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) std::swap(select_, candidate_);
        if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
             ImGui::IsKeyPressed(ImGuiKey_E, false)) && active_ >= 0) {
            a.open = active_;
            grid = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_G, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) view = GridView;
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) view = SurveyView;
    }

    const bool side = beginMain();
    ImGui::BeginChild("##compare", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const float gap = 8.0f, capH = captionHeight(), labelH = ImGui::GetTextLineHeight() + 6;
    const float paneW = std::max(40.0f, (avail.x - gap) * 0.5f), paneH = std::max(40.0f, avail.y - capH - labelH);
    for (int k = 0; k < 2; ++k) {
        const int i = k ? candidate_ : select_;
        const ImVec2 l0(o.x + k * (paneW + gap), o.y), p0(l0.x, l0.y + labelH), p1(p0.x + paneW, p0.y + paneH);
        ImGui::PushID(k);
        dl->AddText(ImVec2(l0.x + 2, l0.y + 2), i >= 0 && i == active_ ? IM_COL32_WHITE : IM_COL32(150, 150, 158, 255),
                    k ? "Candidate" : "Select");
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##pane", ImVec2(paneW, paneH + capH));
        if (i < 0) {
            dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 27, 255));
            dl->AddText(ImVec2(p0.x + 10, p0.y + 10), IM_COL32(150, 150, 158, 255), "No other photo to compare (check the filter)");
            ImGui::PopID();
            continue;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) active_ = i;
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            a.open = i;
            grid = false;
        }
        contextMenu(i, a);
        // Wheel zooms both panes; dragging pans them (in the dragged picture's units).
        const Item& it = *items_[size_t(i)];
        const GLTexture& t = it.preview.valid() ? it.preview : it.tex;
        const float fit = t.valid() ? std::min(paneW / t.width(), paneH / t.height()) : 1.0f;
        const float w = t.valid() ? t.width() * fit * zoom_ : paneW, h = t.valid() ? t.height() * fit * zoom_ : paneH;
        if (hovered && io.MouseWheel != 0.0f) {
            zoom_ = std::clamp(zoom_ * std::pow(1.25f, io.MouseWheel), 1.0f, 8.0f);
            if (zoom_ <= 1.0f) centerX_ = centerY_ = 0.5f;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && zoom_ > 1.0f) {
            centerX_ = std::clamp(centerX_ - io.MouseDelta.x / w, 0.0f, 1.0f);
            centerY_ = std::clamp(centerY_ - io.MouseDelta.y / h, 0.0f, 1.0f);
        }
        drawPreview(dl, i, p0, p1, true);
        if (i == active_) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32_WHITE, 0, 0, 2.0f);
        drawCaption(dl, i, ImVec2(p0.x, p1.y + 4), paneW);
        ImGui::PopID();
    }
    ImGui::EndChild();
    endMain(side);
    return a;
}

LibraryPanel::Actions LibraryPanel::drawSurvey(bool keys) {
    Actions a;
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<int> sel = selection();
    if (std::find(sel.begin(), sel.end(), active_) == sel.end()) active_ = sel.empty() ? -1 : sel[0];
    releasePreviews(sel);

    toolbar(a);
    viewButtons();
    if (sel.size() < 2) {
        ImGui::SameLine();
        ImGui::TextDisabled("Select two or more photos (Ctrl+click in the Grid) to survey them");
    }

    // ---- keys: culling on the active photo, arrows move it, / takes it out of the selection
    const int pos = int(std::find(sel.begin(), sel.end(), active_) - sel.begin());
    int remove = -1;
    if (keys && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && active_ >= 0) {
        cullKeys({active_});
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && pos > 0) active_ = sel[size_t(pos - 1)];
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && pos + 1 < int(sel.size())) active_ = sel[size_t(pos + 1)];
        if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDivide, false)) remove = active_;
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            a.open = active_;
            grid = false;
        }
    }
    if (keys && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        if (ImGui::IsKeyPressed(ImGuiKey_G, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) view = GridView;
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) view = CompareView, select_ = candidate_ = -1;
    }

    const bool side = beginMain();
    ImGui::BeginChild("##survey", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const float gap = 8.0f, capH = captionHeight();
    // The columns that give the largest tiles (for 3:2 photos).
    const int n = int(sel.size());
    int cols = 1;
    float best = 0;
    for (int c = 1; c <= std::max(1, n); ++c) {
        const int rows = (n + c - 1) / c;
        const float w = (avail.x - gap * (c - 1)) / c, h = (avail.y - gap * (rows - 1)) / std::max(1, rows) - capH;
        const float s = std::min(w / 1.5f, h);
        if (s > best) best = s, cols = c;
    }
    const int rows = std::max(1, (n + cols - 1) / cols);
    const float tileW = std::max(40.0f, (avail.x - gap * (cols - 1)) / cols);
    const float tileH = std::max(30.0f, (avail.y - gap * (rows - 1)) / rows - capH);
    for (int k = 0; k < n; ++k) {
        const int i = sel[size_t(k)];
        const ImVec2 p0(o.x + (k % cols) * (tileW + gap), o.y + (k / cols) * (tileH + capH + gap)), p1(p0.x + tileW, p0.y + tileH);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##tile", ImVec2(tileW, tileH + capH));
        const bool hovered = ImGui::IsItemHovered();
        contextMenu(i, a);
        drawPreview(dl, i, p0, p1, false);
        // A cross at the top right takes the photo out of the survey (out of the selection).
        const ImVec2 x0(p1.x - 22, p0.y + 4), x1(p1.x - 4, p0.y + 22);
        const bool overX = hovered && n > 1 && ImGui::IsMouseHoveringRect(x0, x1);
        if (hovered && n > 1) {
            dl->AddRectFilled(x0, x1, overX ? IM_COL32(200, 60, 50, 230) : IM_COL32(0, 0, 0, 160), 3.0f);
            dl->AddLine(ImVec2(x0.x + 5, x0.y + 5), ImVec2(x1.x - 5, x1.y - 5), IM_COL32_WHITE, 2.0f);
            dl->AddLine(ImVec2(x1.x - 5, x0.y + 5), ImVec2(x0.x + 5, x1.y - 5), IM_COL32_WHITE, 2.0f);
        }
        if (ImGui::IsItemClicked()) {
            if (overX) remove = i;
            else active_ = i;
        }
        if (hovered && !overX && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            a.open = i;
            grid = false;
        }
        if (i == active_) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32_WHITE, 0, 0, 2.0f);
        drawCaption(dl, i, ImVec2(p0.x, p1.y + 4), tileW);
        ImGui::PopID();
    }
    ImGui::EndChild();
    endMain(side);
    if (remove >= 0 && n > 1) {
        items_[size_t(remove)]->selected = false;
        if (remove == active_) {
            const int p = int(std::find(sel.begin(), sel.end(), remove) - sel.begin());
            active_ = sel[size_t(p + 1 < n ? p + 1 : p - 1)];
        }
        // The current photo stands in for an empty selection: keep the others selected instead.
        if (remove == current_)
            for (int i : sel)
                if (i != remove) items_[size_t(i)]->selected = true;
    }
    return a;
}

// ---------------------------------------------------------------- metadata

bool LibraryPanel::beginMain() {
    if (!showMetadata) return false;
    const float sideW = 300.0f;
    ImGui::BeginChild("##main", ImVec2(std::max(100.0f, ImGui::GetContentRegionAvail().x - sideW - 6), 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    return true;
}

void LibraryPanel::endMain(bool side) {
    if (!side) return;
    ImGui::EndChild();
    ImGui::SameLine(0, 6);
    ImGui::BeginChild("##metaSide", ImVec2(0, 0), ImGuiChildFlags_Borders);
    drawMetadata();
    ImGui::EndChild();
}

void LibraryPanel::drawMetadataWindow() {
    ImGui::SetNextWindowSize(ImVec2(320, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Metadata###LibraryMetadata", &showMetadata)) drawMetadata();
    ImGui::End();
}

void LibraryPanel::drawMetadata() {
    const std::vector<int> sel = selection();
    if (sel.empty()) {
        ImGui::TextDisabled("No photo selected");
        return;
    }
    // The fields hold what the selection has in common; reloaded when the selection changes.
    auto common = [&](auto get) {
        for (int i : sel)
            if (get(meta(i)) != get(meta(sel[0]))) return false;
        return true;
    };
    const bool sameTitle = common([](const library::Meta& m) { return m.title; });
    const bool sameCaption = common([](const library::Meta& m) { return m.caption; });
    std::string key;
    for (int i : sel) key += photo(i) + "#" + std::to_string(copyOf(i)) + "\n";
    if (key != metaFor_ && !ImGui::IsAnyItemActive()) {
        metaFor_ = key;
        std::snprintf(title_, sizeof title_, "%s", sameTitle ? meta(sel[0]).title.c_str() : "");
        std::snprintf(caption_, sizeof caption_, "%s", sameCaption ? meta(sel[0]).caption.c_str() : "");
        keyword_[0] = 0;
    }

    if (sel.size() == 1) ImGui::TextUnformatted(entryName(photo(sel[0]), copyOf(sel[0])).c_str());
    else ImGui::Text("%d photos selected", int(sel.size()));
    ImGui::Separator();

    // ---- colour label
    const int label = common([](const library::Meta& m) { return m.label; }) ? meta(sel[0]).label : -1;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Label");
    ImGui::SameLine(70);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##label", label < 0 ? "(mixed)" : library::labelName(label))) {
        for (int l = 0; l < library::kLabelCount; ++l) {
            ImGui::PushID(l);
            const ImVec2 c = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##l", l == label)) setLabel(sel, l);
            if (l) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(c.x, c.y + 3), ImVec2(c.x + 10, c.y + ImGui::GetTextLineHeight() - 3), labelColor(l), 2.0f);
            ImGui::GetWindowDrawList()->AddText(ImVec2(c.x + 16, c.y), ImGui::GetColorU32(ImGuiCol_Text), library::labelName(l));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // ---- title and caption: applied when the field is left (Enter, Tab or a click elsewhere)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Title");
    ImGui::SameLine(70);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##title", sameTitle ? "" : "(mixed)", title_, sizeof title_);
    if (ImGui::IsItemDeactivatedAfterEdit())
        for (int i : sel) {
            meta(i).title = title_;
            writeMeta(i);
        }
    ImGui::TextUnformatted("Caption");
    if (!sameCaption) {
        ImGui::SameLine();
        ImGui::TextDisabled("(mixed: typing replaces them all)");
    }
    ImGui::InputTextMultiline("##caption", caption_, sizeof caption_, ImVec2(-1, ImGui::GetTextLineHeight() * 4));
    if (ImGui::IsItemDeactivatedAfterEdit())
        for (int i : sel) {
            meta(i).caption = caption_;
            writeMeta(i);
        }

    // ---- keywords: those of the selection (with how many have each), click one to remove it
    ImGui::Spacing();
    ImGui::TextUnformatted("Keywords");
    std::vector<std::pair<std::string, int>> kws;
    for (int i : sel)
        for (const std::string& k : meta(i).keywords) {
            auto f = std::find_if(kws.begin(), kws.end(), [&](const auto& p) { return lowerAscii(p.first) == lowerAscii(k); });
            if (f == kws.end()) kws.push_back({k, 1});
            else ++f->second;
        }
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    std::string removeKw;
    for (size_t k = 0; k < kws.size(); ++k) {
        std::string text = kws[k].first;
        if (kws[k].second < int(sel.size())) text += " (" + std::to_string(kws[k].second) + ")";
        text += "  x##kw" + std::to_string(k);
        const float w = ImGui::CalcTextSize(text.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
        if (k > 0) {
            ImGui::SameLine(0, 4);
            if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
        }
        if (ImGui::SmallButton(text.c_str())) removeKw = kws[k].first;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove from the selected photos");
    }
    if (kws.empty()) ImGui::TextDisabled("None");
    if (!removeKw.empty())
        for (int i : sel)
            if (meta(i).hasKeyword(removeKw)) {
                meta(i).removeKeyword(removeKw);
                writeMeta(i);
            }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##addKeywords", "Add keywords (a, b, c) and Enter", keyword_, sizeof keyword_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::vector<std::string> add = library::splitKeywords(keyword_);
        for (int i : sel) {
            bool changed = false;
            for (const std::string& k : add)
                if (!meta(i).hasKeyword(k)) meta(i).addKeyword(k), changed = true;
            if (changed) writeMeta(i);
        }
        keyword_[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);  // keep typing more
    }

    // ---- the keywords used in this folder or collection, to click on
    std::vector<std::pair<std::string, int>> all;
    for (const auto& it : items_)
        for (const std::string& k : it->meta.keywords) {
            auto f = std::find_if(all.begin(), all.end(), [&](const auto& p) { return lowerAscii(p.first) == lowerAscii(k); });
            if (f == all.end()) all.push_back({k, 1});
            else ++f->second;
        }
    if (!all.empty() && ImGui::TreeNodeEx("Keyword List", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return lowerAscii(x.first) < lowerAscii(y.first); });
        for (const auto& [k, count] : all) {
            const bool allHave = std::all_of(sel.begin(), sel.end(), [&](int i) { return meta(i).hasKeyword(k); });
            const std::string text = k + "  " + std::to_string(count);
            if (ImGui::Selectable(text.c_str(), allHave) && !allHave)
                for (int i : sel)
                    if (!meta(i).hasKeyword(k)) {
                        meta(i).addKeyword(k);
                        writeMeta(i);
                    }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(allHave ? "The selected photos have it" : "Add to the selected photos");
        }
        ImGui::TreePop();
    }

    // ---- the camera's details (read once per photo)
    if (sel.size() == 1) {
        Item& it = *items_[size_t(sel[0])];
        if (!it.infoRead) {
            exif::readInfo(it.path, it.info);
            it.infoRead = true;
        }
        const exif::PhotoInfo& in = it.info;
        if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto row = [](const char* name, const std::string& value) {
                if (value.empty()) return;
                ImGui::TextDisabled("%s", name);
                ImGui::SameLine(90);
                ImGui::TextUnformatted(value.c_str());
            };
            char buf[64];
            row("Captured", in.captureTime);
            row("Camera", in.make.empty() || in.model.rfind(in.make, 0) == 0 ? in.model : in.make + " " + in.model);
            row("Lens", in.lens);
            std::string exposure;
            if (in.exposureTime > 0) {
                if (in.exposureTime < 0.5f) std::snprintf(buf, sizeof buf, "1/%.0f s", 1.0f / in.exposureTime);
                else std::snprintf(buf, sizeof buf, "%.1f s", in.exposureTime);
                exposure = buf;
            }
            if (in.fNumber > 0) std::snprintf(buf, sizeof buf, "%sf/%.1f", exposure.empty() ? "" : "   ", in.fNumber), exposure += buf;
            row("Exposure", exposure);
            if (in.iso > 0) std::snprintf(buf, sizeof buf, "%.0f", in.iso), row("ISO", buf);
            if (in.focalLength > 0) std::snprintf(buf, sizeof buf, "%.0f mm", in.focalLength), row("Focal Length", buf);
            row("File", pathToU8(u8ToPath(it.path).filename()));
            ImGui::TreePop();
        }
    }
}
