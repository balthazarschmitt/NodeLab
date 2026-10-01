#include "io/ImageCache.h"

#include <algorithm>
#include <cmath>

#include "io/ImageIO.h"

namespace {
std::string entryKey(const std::string& pathU8, const ImageCache::Decode& d) {
    return pathU8 + (d.srgbToLinear ? "|lin" : "|asis") + (d.sceneLinear ? "|scene" : "|legacy") + "|hl" +
           std::to_string(d.rawHighlights);
}

// Full-resolution levels kept for zoomed-in viewing: the full image and one smaller level, or two
// images, without holding every file ever zoomed into (a 24 MP level is 384 MB).
constexpr size_t kKeptLevels = 2;
}  // namespace

ImagePtr ImageCache::get(const std::string& pathU8, bool proxy, std::string* err, const Decode& decode, int proxyEdge) {
    if (pathU8.empty()) {
        if (err) *err = "no file selected";
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    return getLocked(entryKey(pathU8, decode), pathU8, proxy, err, decode, proxyEdge);
}

ImagePtr ImageCache::getLocked(const std::string& key, const std::string& pathU8, bool proxy, std::string* err,
                               const Decode& decode, int proxyEdge) {
    Entry& e = entries_[key];
    if (!e.error.empty()) {
        if (err) *err = e.error;
        return nullptr;
    }
    if (proxy) {
        if (auto it = e.proxies.find(proxyEdge); it != e.proxies.end()) return it->second;
        // A proxy for another viewer size: downscale a bigger one rather than decode again.
        for (const auto& [edge, p] : e.proxies)
            if (edge > proxyEdge || std::max(p->w, p->h) < edge)  // bigger, or already the whole image
                return e.proxies[proxyEdge] = downscaleToFit(p, proxyEdge);
    } else if (ImagePtr full = e.full.lock()) {
        return full;
    }

    // Decode. Only the preview proxy is kept; the full-resolution image (16 bytes per pixel) lives
    // only as long as someone (an export, a kept level) holds it, then is decoded again when next
    // needed.
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
    ImagePtr& p = e.proxies[proxyEdge];
    if (!p) p = downscaleToFit(full, proxyEdge);
    if (proxy) return p;
    e.full = full;
    return full;
}

void ImageCache::levelSize(int fw, int fh, float scale, int& w, int& h) {
    w = fw;
    h = fh;
    if (scale >= 1.0f) return;
    // Same rounding as downscaleToFit, which makes the level.
    const int maxEdge = std::max(1, int(std::lround(std::max(fw, fh) * double(scale))));
    if (std::max(fw, fh) <= maxEdge) return;
    const double s = double(maxEdge) / std::max(fw, fh);
    w = std::max(1, int(std::lround(fw * s)));
    h = std::max(1, int(std::lround(fh * s)));
}

ImagePtr ImageCache::level(const std::string& pathU8, float scale, std::string* err, const Decode& decode) {
    if (pathU8.empty()) {
        if (err) *err = "no file selected";
        return nullptr;
    }
    const std::string key = entryKey(pathU8, decode);
    const std::string lkey = key + "|s" + std::to_string(scale);
    std::lock_guard lock(mutex_);
    for (auto it = levels_.begin(); it != levels_.end(); ++it)
        if (it->first == lkey) {
            auto hit = *it;
            levels_.erase(it);
            levels_.push_front(hit);
            return hit.second;
        }
    ImagePtr full;
    for (const auto& [k, img] : levels_)
        if (k == key + "|s" + std::to_string(1.0f)) full = img;
    if (!full) full = getLocked(key, pathU8, false, err, decode, kProxyEdge);
    if (!full) return nullptr;
    int w, h;
    levelSize(full->w, full->h, scale, w, h);
    ImagePtr img = w == full->w && h == full->h ? full : downscaleToFit(full, std::max(w, h));
    levels_.emplace_front(lkey, img);
    while (levels_.size() > kKeptLevels) levels_.pop_back();
    return img;
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
    levels_.clear();
}
