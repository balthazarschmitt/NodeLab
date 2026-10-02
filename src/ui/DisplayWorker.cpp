#include "ui/DisplayWorker.h"

#include <algorithm>

#include "gpu/Device.h"
#include "gpu/Display.h"

DisplayWorker::DisplayWorker() : thread_([this] { run(); }) {}

DisplayWorker::~DisplayWorker() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void DisplayWorker::submit(Request r) {
    {
        std::lock_guard lock(mutex_);
        std::erase_if(queue_, [&](const Request& q) { return q.slot == r.slot; });
        queue_.push_back(std::move(r));
    }
    cv_.notify_all();
}

std::vector<DisplayWorker::Result> DisplayWorker::poll() {
    std::lock_guard lock(mutex_);
    return std::exchange(done_, {});
}

bool DisplayWorker::busy() const {
    std::lock_guard lock(mutex_);
    return !queue_.empty() || working_ > 0 || !done_.empty();
}

bool DisplayWorker::gpuDisplay(const Request& r, Result& out) {
    if (!gpu::available()) return false;
    try {
        gpu::Scope scope;
        gpu::DisplayResult d = gpu::display(r.gpuScene.empty() ? Value(r.scene) : r.gpuScene, r.cm, r.clipping,
                                            r.histogram, r.keepTexture && gpu::sharesUiContext());
        if (d.w != out.w || d.h != out.h) return false;
        out.bytes = std::move(d.bytes);
        out.texture = std::move(d.texture);
        if (r.histogram) out.histogram.setCounts(d.histogram.data());
        return true;
    } catch (const gpu::Error&) {
        return false;
    }
}

ImagePtr DisplayWorker::download(const Value& v) {
    try {
        gpu::Scope scope;
        const Value cpu = toCpu(v);
        int w = 0, h = 0;
        cpu.size(w, h);
        return toImage(cpu, w, h);
    } catch (const gpu::Error&) {
        return nullptr;
    }
}

void DisplayWorker::run() {
    for (;;) {
        Request r;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            r = std::move(queue_.front());
            queue_.pop_front();
            ++working_;
        }
        Result out;
        out.slot = r.slot;
        out.seq = r.seq;
        out.scene = r.scene;
        if (!r.scene && !r.gpuScene.empty()) r.gpuScene.size(out.w, out.h);
        else if (r.scene) out.w = r.scene->w, out.h = r.scene->h;
        const bool onDevice = out.w > 0 && out.h > 0 && !r.tint && r.gpu && gpuDisplay(r, out);
        if (!onDevice && out.w > 0 && out.h > 0) {
            if (!r.scene) r.scene = download(r.gpuScene);
            if (r.scene && r.tint) {
                out.bytes = tintBytes(*r.scene, r.tintColor[0], r.tintColor[1], r.tintColor[2], r.tintColor[3]);
            } else if (r.scene) {
                const ImagePtr display = colormgmt::displayImage(r.scene, r.cm);
                out.bytes = displayBytes(*display, r.clipping);
                if (r.histogram) out.histogram.compute(*display);
            }
        }
        std::lock_guard lock(mutex_);
        done_.push_back(std::move(out));
        --working_;
    }
}
