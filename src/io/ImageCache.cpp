#include "io/ImageCache.h"

#include "io/ImageIO.h"

ImagePtr ImageCache::get(const std::string& pathU8, bool proxy, std::string* err) {
    if (pathU8.empty()) {
        if (err) *err = "no file selected";
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    auto it = entries_.find(pathU8);
    if (it == entries_.end()) {
        Entry e;
        e.full = loadImage(pathU8, e.error);
        if (e.full) e.proxy = downscaleToFit(e.full, kProxyEdge);
        it = entries_.emplace(pathU8, std::move(e)).first;
    }
    if (!it->second.full) {
        if (err) *err = it->second.error;
        return nullptr;
    }
    return proxy ? it->second.proxy : it->second.full;
}

void ImageCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}
