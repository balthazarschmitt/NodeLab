#pragma once
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

// Runs fn(row) for row in [0, rows) across hardware threads. Rows are handed out
// in small chunks via an atomic counter so uneven workloads balance out.
template <typename Fn>
void parallelFor(int rows, Fn&& fn) {
    if (rows <= 0) return;
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int workers = static_cast<int>(std::min<unsigned>(hw, static_cast<unsigned>(rows)));
    if (workers <= 1 || rows < 16) {
        for (int y = 0; y < rows; ++y) fn(y);
        return;
    }
    const int chunk = std::max(1, rows / (workers * 8));
    std::atomic<int> next{0};
    auto work = [&] {
        for (;;) {
            int start = next.fetch_add(chunk);
            if (start >= rows) break;
            int end = std::min(rows, start + chunk);
            for (int y = start; y < end; ++y) fn(y);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (int i = 0; i < workers - 1; ++i) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}

// Like parallelFor but hands out row ranges: fn(yBegin, yEnd). Useful when each worker needs
// per-chunk setup (e.g. compiling an expression once instead of per row).
template <typename Fn>
void parallelForChunks(int rows, Fn&& fn) {
    if (rows <= 0) return;
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int workers = static_cast<int>(std::min<unsigned>(hw, static_cast<unsigned>(rows)));
    const int chunk = std::max(1, rows / std::max(1, workers * 4));
    std::atomic<int> next{0};
    auto work = [&] {
        for (;;) {
            int start = next.fetch_add(chunk);
            if (start >= rows) break;
            fn(start, std::min(rows, start + chunk));
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < workers - 1; ++i) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}
