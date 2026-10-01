#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/Image.h"
#include "io/ImageIO.h"

// Thread-safe cache of decoded source images, full resolution and preview proxy.
class ImageCache {
public:
    static constexpr int kProxyEdge = 1280;

    // How pixel values are decoded. A file decoded several ways is cached once per way.
    using Decode = DecodeOptions;

    // Returns null on failure; err receives the reason. The proxy is downscaled after decoding,
    // so a linear proxy averages in linear light. RAW proxies come from a fast half-size decode.
    ImagePtr get(const std::string& pathU8, bool proxy, std::string* err = nullptr, const Decode& decode = {});
    // Full-resolution size of a loaded image (false if it has not loaded).
    bool fullSize(const std::string& pathU8, int& w, int& h);
    void clear();

private:
    struct Entry {
        ImagePtr proxy;                     // always kept once loaded
        std::weak_ptr<const Image> full;    // kept only while in use
        int fullW = 0, fullH = 0;
        std::string error;
    };
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;                  // by path and decode options
    std::map<std::string, std::pair<int, int>> fullSizes_;  // by path
};
