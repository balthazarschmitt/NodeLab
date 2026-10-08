#pragma once
// The photo library (File > Open Folder): a folder's photos, each with its edit in a sidecar
// project next to it (IMG_1234.CR3 -> IMG_1234.CR3.refract, as darktable does), so edits travel
// with the photos and a sidecar opens like any other project. Ratings, the pick/reject flag and
// a thumbnail of the edit live in the sidecar's "ui" block, under "library".
#include <array>
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
// Lightroom's colour labels (keys 6-9 set the first four).
enum Label { NoLabel = 0, Red, Yellow, Green, Blue, Purple, kLabelCount };
const char* labelName(int label);

struct Meta {
    int rating = 0;          // 0..5 stars
    int flag = Unflagged;
    int label = NoLabel;
    bool edited = false;     // the graph was changed (a sidecar made only by rating isn't)
    std::string thumb;       // base64 JPEG of the edited result; empty: use the photo's own
    // Lightroom's Metadata panel: a title, a caption and keywords (kept in the order added,
    // without repeats ignoring case).
    std::string title, caption;
    std::vector<std::string> keywords;
    // Lightroom's stacks: photos with the same id are one stack, shown as its first photo until
    // expanded. Empty: not stacked.
    std::string stack;
    nlohmann::json toJson() const;
    static Meta fromJson(const nlohmann::json& j);
    bool hasKeyword(const std::string& k) const;  // ignoring case
    void addKeyword(const std::string& k);        // trimmed; nothing when empty or already there
    void removeKeyword(const std::string& k);
};

// The meta as an XMP packet for exported files, as Lightroom writes it: dc:title,
// dc:description (the caption), dc:subject (keywords), xmp:Rating (-1 for a rejected photo) and
// xmp:Label. Empty when there is nothing to write.
std::string xmpPacket(const Meta& m);

// Keywords typed as "a, b; c": split at commas and semicolons, trimmed, empty ones dropped.
std::vector<std::string> splitKeywords(const std::string& text);
// Whether the photo matches a search: every word of `query` is in its file name, title,
// caption or a keyword, ignoring case.
bool matchesSearch(const std::string& photoU8, const Meta& m, const std::string& query);

// Lightroom's virtual copies: more edits of one photo, each in its own sidecar. Copy 0 is the
// photo's own edit (photo.ext.refract); copy N > 0 lives in photo.ext.copyN.refract.
// NodeLab's sidecars (photo.ext.nlproj, before 1.7) are read where they are: sidecarPath returns
// one when there is no .refract. sidecarSavePath renames it first, for writing.
std::string sidecarPath(const std::string& photoU8, int copy = 0);
std::string sidecarSavePath(const std::string& photoU8, int copy = 0);
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

// Lightroom's collections: named lists of photos (and virtual copies) from any folders, kept in
// %APPDATA%\Refractory\collections.json.
struct Collection {
    std::string name;
    std::vector<Entry> entries;
};
std::vector<Collection> loadCollections();
bool saveCollections(const std::vector<Collection>& c, std::string& err);
void setCollectionsFile(const std::string& pathU8);  // tests use their own
// Adds entries not already in it (creating the collection); returns how many were added.
int addToCollection(std::vector<Collection>& c, const std::string& name, const std::vector<Entry>& entries);

// Duplicate detection: a hash of the file's bytes (the same photo copied), and a 64-bit
// difference hash of the picture (the same photo re-saved, resized or re-encoded), compared by
// how many bits differ.
uint64_t fileHash(const std::string& pathU8);  // 0 when it can't be read
uint64_t pictureHash(const Image& img);
// The picture hashes of img turned 90, 180 and 270 degrees clockwise, to find rotated copies.
std::array<uint64_t, 3> rotatedHashes(const Image& img);
int hashDistance(uint64_t a, uint64_t b);
// Groups of indices whose file hashes are equal (and non-zero) or whose picture hashes are within
// maxBits of each other (and whose aspect ratios match, given as w / h). With `rotated` (from
// rotatedHashes), a photo also matches another turned by a quarter, a half or three quarters,
// with the aspect ratio inverted for the quarter turns. Groups have 2 or more members, in index
// order, and are sorted by their first member.
std::vector<std::vector<int>> groupDuplicates(const std::vector<uint64_t>& files, const std::vector<uint64_t>& pictures,
                                              const std::vector<float>& aspects, int maxBits = 4,
                                              const std::vector<std::array<uint64_t, 3>>& rotated = {});

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
