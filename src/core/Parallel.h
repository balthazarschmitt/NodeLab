#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <stdexcept>

// Thrown when an evaluation is cancelled (a newer edit superseded it, or the app is closing).
struct EvalCancelled : std::runtime_error {
    EvalCancelled() : std::runtime_error("cancelled") {}
};

namespace parallel {

// Worker threads started once and shared by every parallelFor. Starting threads per call cost
// more than the work itself for fast per-pixel nodes, and a graph makes hundreds of such calls.
int workerCount();  // threads that run chunks, including the calling thread

// Runs fn(begin, end) over [0, count) in chunks of `chunk`. The calling thread works too, so
// nested calls (a chunk that itself calls parallelFor) always make progress. Rethrows the first
// exception a chunk threw. Throws EvalCancelled if the current cancel flag was raised.
void run(int count, int chunk, const std::function<void(int, int)>& fn);

// The cancel flag parallel work checks between chunks on this thread (and on the workers
// helping it). Evaluator sets it around each node, so a long node stops soon after a cancel.
const std::atomic<bool>* currentCancel();

// Runs the calling thread below normal priority. Image work uses every core; at the same
// priority as the UI thread it starves it, and the whole window stutters while a graph evaluates.
void lowerThreadPriority();

class CancelScope {
public:
    explicit CancelScope(const std::atomic<bool>* flag);
    ~CancelScope();
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;

private:
    const std::atomic<bool>* prev_;
};

}  // namespace parallel

// Runs fn(row) for row in [0, rows) across the worker threads. Rows are handed out in small
// chunks so uneven workloads balance out.
template <typename Fn>
void parallelFor(int rows, Fn&& fn) {
    if (rows <= 0) return;
    const int workers = parallel::workerCount();
    if (workers <= 1 || rows < 16) {
        for (int y = 0; y < rows; ++y) fn(y);
        return;
    }
    const int chunk = std::max(1, rows / (workers * 8));
    parallel::run(rows, chunk, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) fn(y);
    });
}

// Like parallelFor but hands out row ranges: fn(yBegin, yEnd). Useful when each worker needs
// per-chunk setup (e.g. compiling an expression once instead of per row).
template <typename Fn>
void parallelForChunks(int rows, Fn&& fn) {
    if (rows <= 0) return;
    const int workers = parallel::workerCount();
    const int chunk = std::max(1, rows / std::max(1, workers * 4));
    parallel::run(rows, chunk, [&](int y0, int y1) { fn(y0, y1); });
}
