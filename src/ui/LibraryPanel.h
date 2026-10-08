#pragma once
// The Library panel (File > Open Folder): a filmstrip of a folder's photos with Lightroom's
// culling (ratings, pick / reject flags, a filter). The App opens photos and pastes or exports
// edits; this keeps the list, the selection and the thumbnails, which load on a background
// thread: a RAW's embedded preview, or for edited photos a render of the edit kept in the sidecar.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "io/Exif.h"
#include "io/Library.h"
#include "ui/ImageView.h"

class LibraryPanel {
public:
    LibraryPanel() = default;
    ~LibraryPanel();
    LibraryPanel(const LibraryPanel&) = delete;
    LibraryPanel& operator=(const LibraryPanel&) = delete;

    // Lists the folder's photos (and reads their sidecars' ratings). False if it has none.
    bool open(const std::string& dirU8);
    // Lists a collection's photos that still exist instead (the folder stays, to go back to).
    bool openCollection(const std::string& name);
    bool active() const { return !dir_.empty(); }
    const std::string& folder() const { return dir_; }
    const std::string& collection() const { return collection_; }  // empty: a folder is shown
    int size() const { return int(items_.size()); }
    const std::string& photo(int i) const { return items_[size_t(i)]->path; }
    int copyOf(int i) const { return items_[size_t(i)]->copy; }  // virtual copy number (0: the photo)
    std::string sidecar(int i) const { return library::sidecarPath(photo(i), copyOf(i)); }
    // A new virtual copy listed after entry i (its photo's last copy); returns its index.
    int insertCopy(int i, int copy);
    void removeEntry(int i);
    library::Meta& meta(int i) { return items_[size_t(i)]->meta; }
    const library::Meta& meta(int i) const { return items_[size_t(i)]->meta; }
    int current() const { return current_; }
    // The photo shown in the editor, found by its sidecar (or -1: the project isn't one of them).
    void setCurrentProject(const std::string& projectU8);
    // The selection in list order; the current photo when nothing else is selected.
    std::vector<int> selection() const;
    // The sidecar changed (saved, pasted): reload the thumbnail, rendering the edit if it has none.
    void refresh(int i, bool rerender);
    // Next / previous photo the filter shows, from `from` (-1 when there is none).
    int step(int from, int dir) const;

    // Per frame: takes finished thumbnails (and stores rendered ones in their sidecars).
    void poll();

    struct Actions {
        int open = -1;            // a photo was clicked (or stepped to with the arrow keys)
        bool copy = false, paste = false, exportSelected = false;
        int createCopy = -1, removeCopy = -1;  // Create / Remove Virtual Copy, on this entry
        std::string openCollection, openFolder;  // the Collections menu
    };
    // Draws the panel's contents inside the current window. keys: the culling shortcuts apply
    // (the pointer isn't over the Node Editor and nothing takes text).
    Actions draw(bool keys);
    // Lightroom's Library views, filling the window (`grid` is whether one is shown):
    // - Grid (G): the whole folder as cards, to look over an import, select photos and rate them.
    //   A click selects (Ctrl / Shift add), a double-click or Enter / E opens the photo and leaves
    //   the grid, as does Escape without opening one.
    // - Compare (C): two photos side by side, the Select and the Candidate, zoomed and panned
    //   together. The arrows change the Candidate; Up makes it the Select, Down swaps them.
    // - Survey (N): the selected photos tiled, to narrow a selection down.
    // In Compare and Survey the culling keys rate the active photo (the one outlined).
    enum View { GridView, CompareView, SurveyView };
    bool grid = false;
    int view = GridView;
    Actions drawGrid(bool keys);
    // Lightroom's Metadata panel for the selection: colour label, title, caption, keywords, and
    // the camera's details. A window in the editor; a sidebar in the Library views.
    bool showMetadata = false;
    void drawMetadataWindow(const char* title = "Metadata###LibraryMetadata");  // its workspace's window
    // Lightroom's Sort order (the toolbar's Sort menu), kept in the preferences.
    enum Sort { ByName, ByCaptureTime, ByFileType, ByRating, ByPick, ByEditTime, ByCamera, ByLens, ByIso, ByFocalLength, kSortCount };
    int sortBy = ByName;
    bool sortDescending = false;
    // Sorts the entries by sortBy, keeping the current photo and the selection.
    void sort();
    std::string status;     // set when something worth telling happened (for the status bar)
    bool canPaste = false;  // an edit has been copied

private:
    struct Item {
        std::string path;
        int copy = 0;
        library::Meta meta;
        exif::PhotoInfo info;  // read when a sort needs it
        bool infoRead = false;
        GLTexture tex;
        bool requested = false;
        bool selected = false;
        GLTexture preview;  // large, for Compare and Survey
        bool previewRequested = false;
        int dupGroup = 0;   // 1.. when Find Duplicates put it in a group
    };
    struct Job {
        int index;
        uint64_t gen;
        std::string path;
        int copy;
        library::Meta meta;
        bool render;
        int edge;
    };
    struct Result {
        int index;
        uint64_t gen;
        std::string path;  // with copy: finds the entry again if the list was sorted meanwhile
        int copy;
        ImagePtr image;
        std::string store;  // a rendered edit's thumbnail, for the sidecar
        bool preview;
    };

    bool passes(int i) const;
    // The entries the filter shows, in order (duplicates grouped after Find Duplicates).
    std::vector<int> shown() const;
    void toolbar(Actions& a);
    void collectionsMenu(Actions& a);
    void photoMenu();
    Actions drawCompare(bool keys);
    Actions drawSurvey(bool keys);
    void drawMetadata();
    // Entry i's large preview fitted into b0..b1; zoomed: with the compare view's zoom and centre.
    void drawPreview(ImDrawList* dl, int i, ImVec2 b0, ImVec2 b1, bool zoomed);
    // The name, flag, label and stars under a large preview, from p0, w wide.
    void drawCaption(ImDrawList* dl, int i, ImVec2 p0, float w) const;
    static float captionHeight();
    void viewButtons();
    // Frees the large previews except those of `keep` (and drops their queued renders).
    void releasePreviews(const std::vector<int>& keep = {});
    Actions drawStrip(Actions a);  // the filmstrip, inside its child window
    // A photo's thumbnail fitted into b0..b1, with its flag, label, stack and edited badges.
    void drawThumb(ImDrawList* dl, int i, ImVec2 b0, ImVec2 b1) const;
    void cullKeys(const std::vector<int>& to);  // 0-5, P, X, U, 6-9 on these entries
    void contextMenu(int i, Actions& a);  // right-click on an entry
    void select(int i, bool ctrl, bool shift);
    void setRating(const std::vector<int>& to, int rating);
    void setFlag(const std::vector<int>& to, int flag);
    void setLabel(const std::vector<int>& to, int label);
    void writeMeta(int i);
    void request(int i, bool render, int edge = library::kThumbEdge);
    void work();

    // Stacks: each stack's top entry and size.
    struct StackInfo {
        int top = -1, count = 0;
    };
    void updateStacks();
    void keepStacksTogether();  // members right after their top, in list order
    void groupStack();
    void unstack();
    void toggleStack(int i);
    const StackInfo* stackOf(int i) const;  // null unless entry i is in a stack of 2 or more
    // The selection, with the hidden members of collapsed stacks whose top is selected.
    std::vector<int> selectionWithStacks() const;

    void loadEntries(std::vector<library::Entry> entries);
    // Puts the entries in this order (a permutation), keeping the current, selected and compared ones.
    void reorder(const std::vector<Item*>& order);
    // The Library views' area beside the Metadata sidebar (when shown); true: endMain closes it.
    bool beginMain();
    void endMain(bool side);
    void findDuplicates();
    void addSelectionTo(const std::string& collection);

    std::string dir_;
    std::string folder_;      // the folder opened last (dir_ too, unless a collection is shown)
    std::string collection_;
    std::vector<library::Collection> collections_;
    char newCollection_[128] = "";
    char search_[128] = "";
    std::set<std::string> expanded_;           // stacks shown open
    std::map<std::string, StackInfo> stacks_;  // by id, those of 2 or more entries
    std::vector<std::unique_ptr<Item>> items_;
    int current_ = -1;
    int anchor_ = -1;  // Shift+click selects from here
    int filter_ = 0;   // see kFilters in the .cpp
    bool scrollToCurrent_ = false;
    int focus_ = -1;          // the grid's keyboard position
    bool scrollToFocus_ = false;
    float gridSize_ = 180.0f; // grid cell width, in pixels (Ctrl+wheel or the slider)
    // Compare: the Select and the Candidate; Compare and Survey: the active photo, which the
    // culling keys rate. The zoom (1 = fitted) and centre (0..1 of the picture) are shared.
    int select_ = -1, candidate_ = -1, active_ = -1;
    float zoom_ = 1.0f, centerX_ = 0.5f, centerY_ = 0.5f;
    bool previewsShown_ = false;  // Compare or Survey drew large previews
    // Metadata panel: the text fields being edited, reloaded when the selection changes.
    std::string metaFor_;
    char title_[256] = "", caption_[1024] = "", keyword_[256] = "";

    // Find Duplicates runs on its own thread; poll() takes its groups.
    std::thread dupThread_;
    std::atomic<bool> dupRunning_{false}, dupStop_{false};
    std::atomic<int> dupDone_{0}, dupTotal_{0};
    struct DupResult {
        uint64_t gen = 0;
        std::vector<std::string> paths;
        std::vector<std::vector<int>> groups;
        bool ready = false;
    } dupResult_;  // guarded by mutex_

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    uint64_t gen_ = 0;  // bumped per folder, so a previous folder's results are dropped
    bool stop_ = false;
};
