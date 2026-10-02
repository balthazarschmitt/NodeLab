#pragma once
// The Library panel (File > Open Folder): a filmstrip of a folder's photos with Lightroom's
// culling (ratings, pick / reject flags, a filter). The App opens photos and pastes or exports
// edits; this keeps the list, the selection and the thumbnails, which load on a background
// thread: a RAW's embedded preview, or for edited photos a render of the edit kept in the sidecar.
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    bool active() const { return !dir_.empty(); }
    const std::string& folder() const { return dir_; }
    int size() const { return int(items_.size()); }
    const std::string& photo(int i) const { return items_[size_t(i)]->path; }
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
    };
    // Draws the panel's contents inside the current window. keys: the culling shortcuts apply
    // (the pointer isn't over the Node Editor and nothing takes text).
    Actions draw(bool keys);
    // Lightroom's Grid view (G): the whole folder as cards filling a window, to look over an
    // import, select photos and rate them. A click selects (Ctrl / Shift add), a double-click
    // or Enter / E opens the photo and leaves the grid, as does Escape without opening one.
    bool grid = false;
    Actions drawGrid(bool keys);
    std::string status;     // set when something worth telling happened (for the status bar)
    bool canPaste = false;  // an edit has been copied

private:
    struct Item {
        std::string path;
        library::Meta meta;
        GLTexture tex;
        bool requested = false;
        bool selected = false;
    };
    struct Job {
        int index;
        uint64_t gen;
        std::string path;
        library::Meta meta;
        bool render;
    };
    struct Result {
        int index;
        uint64_t gen;
        ImagePtr image;
        std::string store;  // a rendered edit's thumbnail, for the sidecar
    };

    bool passes(const Item& it) const;
    void toolbar(Actions& a);
    Actions drawStrip(Actions a);  // the filmstrip, inside its child window
    // A photo's thumbnail fitted into b0..b1, with its flag and edited badges.
    void drawThumb(ImDrawList* dl, const Item& it, ImVec2 b0, ImVec2 b1) const;
    void cullKeys();  // 0-5, P, X, U on the selection
    void select(int i, bool ctrl, bool shift);
    void setRating(int rating);
    void setFlag(int flag);
    void writeMeta(int i);
    void request(int i, bool render);
    void work();

    std::string dir_;
    std::vector<std::unique_ptr<Item>> items_;
    int current_ = -1;
    int anchor_ = -1;  // Shift+click selects from here
    int filter_ = 0;   // see kFilters in the .cpp
    bool scrollToCurrent_ = false;
    int focus_ = -1;          // the grid's keyboard position
    bool scrollToFocus_ = false;
    float gridSize_ = 180.0f; // grid cell width, in pixels (Ctrl+wheel or the slider)

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    uint64_t gen_ = 0;  // bumped per folder, so a previous folder's results are dropped
    bool stop_ = false;
};
