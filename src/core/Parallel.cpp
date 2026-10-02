#include "core/Parallel.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace parallel {

namespace {

thread_local const std::atomic<bool>* tlsCancel = nullptr;

struct Job {
    const std::function<void(int, int)>* fn = nullptr;
    int count = 0, chunk = 1;
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<int> next{0};
    int active = 0;  // workers inside work(); guarded by Pool::mutex_
    std::atomic<bool> cancelled{false};
    std::mutex errMutex;
    std::exception_ptr err;
};

class Pool {
public:
    Pool() {
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned i = 1; i < hw; ++i)
            threads_.emplace_back([this] {
                lowerThreadPriority();
                loop();
            });
    }
    ~Pool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }

    int size() const { return int(threads_.size()) + 1; }

    void run(Job& j) {
        {
            std::lock_guard lock(mutex_);
            jobs_.push_back(&j);
        }
        cv_.notify_all();
        work(j);
        std::unique_lock lock(mutex_);
        // No new helpers after this; wait for the ones still finishing a chunk (j is on our stack).
        std::erase(jobs_, &j);
        done_.wait(lock, [&] { return j.active == 0; });
    }

private:
    static void work(Job& j) {
        CancelScope scope(j.cancel);  // nested parallel work inside a chunk sees the same flag
        for (;;) {
            if (j.cancel && j.cancel->load(std::memory_order_relaxed)) {
                j.cancelled = true;
                j.next = j.count;
                return;
            }
            const int start = j.next.fetch_add(j.chunk);
            if (start >= j.count) return;
            try {
                (*j.fn)(start, std::min(j.count, start + j.chunk));
            } catch (...) {
                std::lock_guard lock(j.errMutex);
                if (!j.err) j.err = std::current_exception();
                j.next = j.count;
            }
        }
    }

    Job* findJob() {
        // Newest first: a nested job (from inside a chunk) finishes before its parent can.
        for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it)
            if ((*it)->next.load() < (*it)->count) return *it;
        return nullptr;
    }

    void loop() {
        std::unique_lock lock(mutex_);
        for (;;) {
            Job* j = nullptr;
            cv_.wait(lock, [&] { return stop_ || (j = findJob()) != nullptr; });
            if (stop_) return;
            ++j->active;
            lock.unlock();
            work(*j);
            lock.lock();
            if (--j->active == 0) done_.notify_all();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cv_, done_;
    std::vector<Job*> jobs_;
    bool stop_ = false;
};

Pool& pool() {
    static Pool p;
    return p;
}

}  // namespace

int workerCount() { return pool().size(); }

void lowerThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void run(int count, int chunk, const std::function<void(int, int)>& fn) {
    if (count <= 0) return;
    Job j;
    j.fn = &fn;
    j.count = count;
    j.chunk = std::max(1, chunk);
    j.cancel = tlsCancel;
    if (count <= j.chunk || pool().size() <= 1) {
        CancelScope scope(j.cancel);
        if (j.cancel && j.cancel->load()) throw EvalCancelled();
        fn(0, count);
    } else {
        pool().run(j);
    }
    if (j.err) std::rethrow_exception(j.err);
    if (j.cancelled) throw EvalCancelled();
}

const std::atomic<bool>* currentCancel() { return tlsCancel; }

namespace {

// Runs fn(begin, end) over [0, n) in 1 MB blocks across the workers; small buffers stay on this
// thread. Never cancelled: allocating or copying an image shouldn't throw EvalCancelled.
template <typename Fn>
void blocks(size_t n, Fn&& fn) {
    constexpr size_t kBlock = size_t(1) << 18;  // floats
    if (n <= kBlock * 2 || pool().size() <= 1) {
        fn(size_t(0), n);
        return;
    }
    CancelScope scope(nullptr);
    run(int((n + kBlock - 1) / kBlock), 1, [&](int b, int e) { fn(size_t(b) * kBlock, std::min(n, size_t(e) * kBlock)); });
}

}  // namespace

void zeroFill(float* p, size_t n) {
    blocks(n, [&](size_t b, size_t e) { std::fill(p + b, p + e, 0.0f); });
}

void copyFloats(float* dst, const float* src, size_t n) {
    blocks(n, [&](size_t b, size_t e) { std::copy(src + b, src + e, dst + b); });
}

CancelScope::CancelScope(const std::atomic<bool>* flag) : prev_(tlsCancel) { tlsCancel = flag; }
CancelScope::~CancelScope() { tlsCancel = prev_; }

}  // namespace parallel
