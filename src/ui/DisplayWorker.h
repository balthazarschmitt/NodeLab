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
#include "core/Value.h"
#include "gpu/Device.h"
#include "ui/ImageView.h"

class DisplayWorker {
public:
    struct Request {
        int slot = 0;       // the caller's: which texture this is for
        uint64_t seq = 0;   // the caller's, copied to the result (to drop superseded ones)
        ImagePtr scene;     // the node's output
        // The output while it is still on the GPU device. scene may then be null: a result left
        // on the device isn't downloaded unless the CPU needs it (a tint, or a device error).
        Value gpuScene;
        // Convert on the GPU device (gpu::display): from gpuScene, else from an upload of
        // scene. Device errors fall back to the CPU.
        bool gpu = false;
        // With gpu: keep the display texture on the device for a viewer in the shared context
        // (Result::texture) instead of reading back bytes.
        bool keepTexture = false;
        ColorManagement cm;
        bool clipping = false;
        int gamut = -1;  // soft proofing: mark colours outside this outspace::Space
        bool histogram = false;
        bool tint = false;  // a mask: a flat colour whose opacity follows the red channel
        float tintColor[4] = {1.0f, 0.25f, 0.2f, 0.45f};
    };
    struct Result {
        int slot = 0;
        uint64_t seq = 0;
        ImagePtr scene;
        std::vector<unsigned char> bytes;
        gpu::TexturePtr texture;  // instead of bytes, when kept on the device
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
    // A result left on the GPU device, as an image (null if the device fails). Waits for the
    // device, which an evaluation may hold.
    static ImagePtr download(const Value& v);

private:
    void run();
    // Fills out's bytes and histogram on the GPU device; false to use the CPU instead.
    static bool gpuDisplay(const Request& r, Result& out);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::vector<Result> done_;
    int working_ = 0;
    bool quit_ = false;
    std::thread thread_;
};
