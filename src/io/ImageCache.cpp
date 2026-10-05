#include "io/ImageCache.h"

#include <algorithm>
#include <cmath>

#include "io/ImageIO.h"

namespace {
std::string entryKey(const std::string& pathU8, const ImageCache::Decode& d) {
    return pathU8 + (d.srgbToLinear ? "|lin" : "|asis") + (d.sceneLinear ? "|scene" : "|legacy") + "|hl" +
           std::to_string(d.rawHighlights) + (d.embeddedProfile ? "|icc" : "");
}

// Full-resolution levels kept for zoomed-in viewing: the full image and one smaller level, or two
// images, without holding every file ever zoomed into (a 24 MP level is 384 MB).
constexpr size_t kKeptLevels = 2;
}  // namespace

ImagePtr ImageCache::lookupLocked(Entry& e, bool proxy, int proxyEdge, std::string* err, bool& failed) {
    failed = false;
    if (!e.error.empty()) {
        if (err) *err = e.error;
        failed = true;
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
    return nullptr;
}

ImagePtr ImageCache::cached(const std::string& pathU8, const Decode& decode, int proxyEdge) {
    if (pathU8.empty()) return nullptr;
    std::lock_guard lock(mutex_);
    auto it = entries_.find(entryKey(pathU8, decode));
    if (it == entries_.end()) return nullptr;
    // Only an exact proxy: making another size downscales, which is the evaluator's work.
    auto p = it->second.proxies.find(proxyEdge);
    return p == it->second.proxies.end() ? nullptr : p->second;
}

ImagePtr ImageCache::get(const std::string& pathU8, bool proxy, std::string* err, const Decode& decode, int proxyEdge) {
    if (pathU8.empty()) {
        if (err) *err = "no file selected";
        return nullptr;
    }
    const std::string key = entryKey(pathU8, decode);
    std::unique_lock lock(mutex_);
    for (;;) {
        bool failed = false;
        if (ImagePtr hit = lookupLocked(entries_[key], proxy, proxyEdge, err, failed)) return hit;
        if (failed) return nullptr;
        // Another thread is decoding this file: its result may serve this request too.
        if (!decoding_.count(key)) break;
        decoded_.wait(lock);
    }
    decoding_.insert(key);
    const bool needProxy = !entries_[key].proxies.count(proxyEdge);
    lock.unlock();

    // Decode, unlocked. Only the preview proxy is kept; the full-resolution image (16 bytes per
    // pixel) lives only as long as someone (an export, a kept level) holds it, then is decoded
    // again when next needed.
    std::string loadErr;
    int fw = 0, fh = 0;
    ImagePtr full, proxyImg;
    try {
        // A proxy request may get a smaller preview decode (half-size RAW), which is only downscaled.
        full = loadImage(pathU8, loadErr, decode, proxy, &fw, &fh);
        if (full && needProxy) proxyImg = downscaleToFit(full, proxyEdge);
    } catch (...) {
        lock.lock();
        decoding_.erase(key);
        decoded_.notify_all();
        throw;
    }

    lock.lock();
    decoding_.erase(key);
    decoded_.notify_all();
    Entry& e = entries_[key];
    if (!full) {
        e.error = loadErr.empty() ? "could not load image" : loadErr;
        if (err) *err = e.error;
        return nullptr;
    }
    e.fullW = fw;
    e.fullH = fh;
    fullSizes_[pathU8] = {fw, fh};
    ImagePtr& p = e.proxies[proxyEdge];
    if (!p) p = proxyImg ? proxyImg : downscaleToFit(full, proxyEdge);
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
    const std::string fullKey = key + "|s" + std::to_string(1.0f);
    ImagePtr full;
    {
        std::lock_guard lock(mutex_);
        for (auto it = levels_.begin(); it != levels_.end(); ++it)
            if (it->first == lkey) {
                auto hit = *it;
                levels_.erase(it);
                levels_.push_front(hit);
                return hit.second;
            }
        for (const auto& [k, img] : levels_)
            if (k == fullKey) full = img;
    }
    // Decoding and downscaling happen unlocked; a level made twice meanwhile is kept once.
    if (!full) full = get(pathU8, false, err, decode, kProxyEdge);
    if (!full) return nullptr;
    int w, h;
    levelSize(full->w, full->h, scale, w, h);
    ImagePtr img = w == full->w && h == full->h ? full : downscaleToFit(full, std::max(w, h));
    std::lock_guard lock(mutex_);
    for (const auto& [k, other] : levels_)
        if (k == lkey) return other;
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

void ImageCache::retain(const std::vector<std::string>& pathsU8) {
    std::lock_guard lock(mutex_);
    auto kept = [&](const std::string& key) {
        // Keys are the path, then '|' and the decode options (paths can't contain '|').
        const std::string path = key.substr(0, key.find('|'));
        return std::find(pathsU8.begin(), pathsU8.end(), path) != pathsU8.end();
    };
    // Files being decoded keep their entry: the decoding thread fills it in.
    std::erase_if(entries_, [&](const auto& e) { return !kept(e.first) && !decoding_.count(e.first); });
    std::erase_if(fullSizes_, [&](const auto& e) { return !kept(e.first); });
    const std::string current = pathsU8.empty() ? std::string() : pathsU8.front();
    std::erase_if(levels_, [&](const auto& e) { return e.first.substr(0, e.first.find('|')) != current; });
}

void ImageCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    fullSizes_.clear();
    levels_.clear();
}
