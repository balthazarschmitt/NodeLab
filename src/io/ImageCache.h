#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/Image.h"

// Thread-safe cache of decoded source images, full resolution and preview proxy.
class ImageCache {
public:
    static constexpr int kProxyEdge = 1280;

    // Returns null on failure; err receives the reason.
    ImagePtr get(const std::string& pathU8, bool proxy, std::string* err = nullptr);
    void clear();

private:
    struct Entry {
        ImagePtr proxy;                     // always kept once loaded
        std::weak_ptr<const Image> full;    // kept only while in use
        std::string error;
    };
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};
