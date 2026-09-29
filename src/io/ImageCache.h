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
    std::map<std::string, Entry> entries_;
};
