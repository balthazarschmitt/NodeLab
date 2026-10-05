#pragma once
// The photo library (File > Open Folder): a folder's photos, each with its edit in a sidecar
// project next to it (IMG_1234.CR3 -> IMG_1234.CR3.nlproj, as darktable does), so edits travel
// with the photos and a sidecar opens like any other project. Ratings, the pick/reject flag and
// a thumbnail of the edit live in the sidecar's "ui" block, under "library".
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Image.h"

class Graph;

namespace library {

constexpr int kThumbEdge = 256;  // long edge of thumbnails, in pixels

// Lightroom's flags.
enum Flag { Rejected = -1, Unflagged = 0, Picked = 1 };

struct Meta {
    int rating = 0;          // 0..5 stars
    int flag = Unflagged;
    bool edited = false;     // the graph was changed (a sidecar made only by rating isn't)
    std::string thumb;       // base64 JPEG of the edited result; empty: use the photo's own
    nlohmann::json toJson() const;
    static Meta fromJson(const nlohmann::json& j);
};

// Lightroom's virtual copies: more edits of one photo, each in its own sidecar. Copy 0 is the
// photo's own edit (photo.ext.nlproj); copy N > 0 lives in photo.ext.copyN.nlproj.
std::string sidecarPath(const std::string& photoU8, int copy = 0);
bool hasSidecar(const std::string& photoU8, int copy = 0);

// The folder's images (not sidecars or other files), sorted by name ignoring case.
std::vector<std::string> listFolder(const std::string& dirU8);

// A library entry: a photo, or one of its virtual copies.
struct Entry {
    std::string photo;
    int copy = 0;
};
// The folder's photos, each followed by its virtual copies (in number order).
std::vector<Entry> listEntries(const std::string& dirU8);

// Lightroom's Create Virtual Copy: a new copy of the photo whose edit starts as `fromCopy`'s.
// Returns its number, or -1 (with err).
int createVirtualCopy(const std::string& photoU8, int fromCopy, std::string& err);
// Removes a virtual copy's sidecar (to the Recycle Bin on Windows). Copy 0, the photo's own
// edit, is never removed.
bool removeVirtualCopy(const std::string& photoU8, int copy, std::string& err);
void setUseRecycleBin(bool on);  // tests delete outright

// Meta from the photo's sidecar; defaults (and false) when it has none or it can't be read.
bool readMeta(const std::string& photoU8, Meta& out, int copy = 0);
// Stores meta in the sidecar, keeping its graph; a photo without one gets one with the default
// graph (rating a photo is enough to create it). A sidecar that exists but can't be read (damaged,
// or locked) is left alone and the call fails.
bool writeMeta(const std::string& photoU8, const Meta& m, std::string& err, int copy = 0);

// The graph a photo starts with: Image Input -> Denoise -> Basic -> Output, scene-linear. RAWs
// get the colour noise reduction Lightroom applies by default (Color 25) and the AgX view, which
// keeps the highlights a RAW holds above 1; other photos start with Denoise off.
void defaultGraph(Graph& g, const std::string& photoU8);
// The view transform and look defaultGraph gives photos other than RAWs (Preferences > New
// Projects; Standard by default). A RAW gets AgX whatever this is, unless it is AgX with a look.
void setDefaultView(int view, int look);

// The photo's edit as graph JSON with absolute paths (for exports): its sidecar's graph, or the
// default one.
nlohmann::json graphFor(const std::string& photoU8, std::string& err, int copy = 0);

// Points an edit at another photo: its Image Input for `source` (or the first one) gets `target`.
// False when the graph has no Image Input.
bool retargetEdit(Graph& g, const std::string& sourceU8, const std::string& targetU8);

// Copy / paste edit: writes `target`'s sidecar with `graph` (JSON, absolute paths), its Image
// Input for `source` (or the first one) pointed at `target`. The target keeps its own rating and
// flag; its thumbnail is dropped, since it showed the old edit.
bool pasteEdit(const nlohmann::json& graph, const std::string& sourceU8, const std::string& targetU8, std::string& err,
               int targetCopy = 0);

// The photo itself for a thumbnail: a RAW's embedded preview, or the decoded image, upright and
// display-encoded, at most `edge` pixels long.
ImagePtr loadThumbnail(const std::string& photoU8, int edge, std::string& err);
// The edit rendered on the CPU at a small proxy, through the view transform.
ImagePtr renderThumbnail(const Graph& g, int edge, std::string& err);

std::string encodeThumb(const Image& img);  // base64 JPEG
ImagePtr decodeThumb(const std::string& b64);

std::string base64Encode(const std::vector<unsigned char>& bytes);
std::vector<unsigned char> base64Decode(const std::string& text);

}  // namespace library
