#pragma once
// Prepares viewer textures off the UI thread: the view transform, the 8-bit conversion (with
// clipping warnings) and the histogram. Done on the UI thread they took tens of milliseconds per
// result, which froze the whole window while a slider streamed results; now the UI only uploads
// finished bytes.
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/ColorManagement.h"
#include "core/Image.h"
#include "ui/ImageView.h"

class DisplayWorker {
public:
    struct Request {
        int slot = 0;       // the caller's: which texture this is for
        uint64_t seq = 0;   // the caller's, copied to the result (to drop superseded ones)
        ImagePtr scene;     // the node's output
        ColorManagement cm;
        bool clipping = false;
        bool histogram = false;
        bool tint = false;  // a mask: a flat colour whose opacity follows the red channel
        float tintColor[4] = {1.0f, 0.25f, 0.2f, 0.45f};
    };
    struct Result {
        int slot = 0;
        uint64_t seq = 0;
        ImagePtr scene, display;
        std::vector<unsigned char> bytes;
        int w = 0, h = 0;
        Histogram histogram;  // valid when requested
    };

    DisplayWorker();
    ~DisplayWorker();
    DisplayWorker(const DisplayWorker&) = delete;
    DisplayWorker& operator=(const DisplayWorker&) = delete;

    // Queues r, replacing a queued request for the same slot.
    void submit(Request r);
    std::vector<Result> poll();
    // Requests queued, being prepared or not yet polled.
    bool busy() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::vector<Result> done_;
    int working_ = 0;
    bool quit_ = false;
    std::thread thread_;
};
