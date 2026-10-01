#include "ui/DisplayWorker.h"

#include <algorithm>

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
        if (r.scene && !r.scene->empty()) {
            out.w = r.scene->w;
            out.h = r.scene->h;
            if (r.tint) {
                out.bytes = tintBytes(*r.scene, r.tintColor[0], r.tintColor[1], r.tintColor[2], r.tintColor[3]);
            } else {
                out.display = colormgmt::displayImage(r.scene, r.cm);
                out.bytes = displayBytes(*out.display, r.clipping);
                if (r.histogram) out.histogram.compute(*out.display);
            }
        }
        std::lock_guard lock(mutex_);
        done_.push_back(std::move(out));
        --working_;
    }
}
