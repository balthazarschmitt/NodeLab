#pragma once
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/Image.h"
#include "io/ImageIO.h"

// Thread-safe cache of decoded source images: preview proxies, and full-resolution levels for
// viewing part of an image zoomed in.
class ImageCache {
public:
    static constexpr int kProxyEdge = 1280;

    // How pixel values are decoded. A file decoded several ways is cached once per way.
    using Decode = DecodeOptions;

    // Returns null on failure; err receives the reason. The proxy (long edge proxyEdge) is
    // downscaled after decoding, so a linear proxy averages in linear light. RAW proxies come from
    // a fast half-size decode.
    ImagePtr get(const std::string& pathU8, bool proxy, std::string* err = nullptr, const Decode& decode = {},
                 int proxyEdge = kProxyEdge);
    // The full-resolution image scaled by `scale` (1, 1/2, 1/4...), for evaluating the region of it
    // a zoomed-in viewer shows. Unlike get(), the last few levels are kept, so panning around
    // doesn't decode the file again.
    ImagePtr level(const std::string& pathU8, float scale, std::string* err = nullptr, const Decode& decode = {});
    // Size of level(scale) for a full-resolution image of fw x fh.
    static void levelSize(int fw, int fh, float scale, int& w, int& h);
    // Full-resolution size of a loaded image (false if it has not loaded).
    bool fullSize(const std::string& pathU8, int& w, int& h);
    void clear();

private:
    struct Entry {
        std::map<int, ImagePtr> proxies;    // by long edge; always kept once loaded
        std::weak_ptr<const Image> full;    // kept only while in use
        int fullW = 0, fullH = 0;
        std::string error;
    };
    ImagePtr getLocked(const std::string& key, const std::string& pathU8, bool proxy, std::string* err,
                       const Decode& decode, int proxyEdge);

    std::mutex mutex_;
    std::map<std::string, Entry> entries_;                  // by path and decode options
    std::map<std::string, std::pair<int, int>> fullSizes_;  // by path
    std::deque<std::pair<std::string, ImagePtr>> levels_;   // most recent first
};
