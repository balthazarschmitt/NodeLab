#include "io/ImageCache.h"

#include "io/ImageIO.h"

namespace {
std::string entryKey(const std::string& pathU8, const ImageCache::Decode& d) {
    return pathU8 + (d.srgbToLinear ? "|lin" : "|asis") + (d.sceneLinear ? "|scene" : "|legacy") + "|hl" +
           std::to_string(d.rawHighlights);
}
}  // namespace

ImagePtr ImageCache::get(const std::string& pathU8, bool proxy, std::string* err, const Decode& decode) {
    if (pathU8.empty()) {
        if (err) *err = "no file selected";
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    Entry& e = entries_[entryKey(pathU8, decode)];
    if (!e.error.empty()) {
        if (err) *err = e.error;
        return nullptr;
    }
    if (proxy && e.proxy) return e.proxy;
    if (!proxy)
        if (ImagePtr full = e.full.lock()) return full;

    // Decode. Only the preview proxy is kept; the full-resolution image (16 bytes per pixel) lives
    // only as long as someone (an export) holds it, then is decoded again when next needed.
    std::string loadErr;
    int fw = 0, fh = 0;
    // A proxy request may get a smaller preview decode (half-size RAW), which is only downscaled.
    std::shared_ptr<const Image> full = loadImage(pathU8, loadErr, decode, proxy, &fw, &fh);
    if (!full) {
        e.error = loadErr.empty() ? "could not load image" : loadErr;
        if (err) *err = e.error;
        return nullptr;
    }
    e.fullW = fw;
    e.fullH = fh;
    fullSizes_[pathU8] = {fw, fh};
    if (!e.proxy) e.proxy = downscaleToFit(full, kProxyEdge);
    if (proxy) return e.proxy;
    e.full = full;
    return full;
}

bool ImageCache::fullSize(const std::string& pathU8, int& w, int& h) {
    std::lock_guard lock(mutex_);
    auto it = fullSizes_.find(pathU8);
    if (it == fullSizes_.end() || it->second.first <= 0) return false;
    w = it->second.first;
    h = it->second.second;
    return true;
}

void ImageCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    fullSizes_.clear();
}
