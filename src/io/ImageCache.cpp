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
    if (!e.proxy) e.proxy = downscaleToFit(full, kProxyEdge);
    if (proxy) return e.proxy;
    e.full = full;
    return full;
}

void ImageCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}
