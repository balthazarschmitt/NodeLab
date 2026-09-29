#include "io/ImageCache.h"

#include "io/ImageIO.h"

ImagePtr ImageCache::get(const std::string& pathU8, bool proxy, std::string* err) {
    if (pathU8.empty()) {
        if (err) *err = "no file selected";
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    Entry& e = entries_[pathU8];
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
    std::shared_ptr<const Image> full = loadImage(pathU8, loadErr);
    if (!full) {
        e.error = loadErr.empty() ? "could not load image" : loadErr;
        if (err) *err = e.error;
        return nullptr;
    }
    e.fullW = full->w;
    e.fullH = full->h;
    if (!e.proxy) e.proxy = downscaleToFit(full, kProxyEdge);
    if (proxy) return e.proxy;
    e.full = full;
    return full;
}

bool ImageCache::fullSize(const std::string& pathU8, int& w, int& h) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(pathU8);
    if (it == entries_.end() || it->second.fullW <= 0) return false;
    w = it->second.fullW;
    h = it->second.fullH;
    return true;
}

void ImageCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}
