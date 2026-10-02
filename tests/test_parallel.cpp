// Thread pool behaviour and the cache-friendly blur.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>
#include <stdexcept>

#include "core/Parallel.h"
#include "nodes/ImageOps.h"

TEST_CASE("parallelFor covers every row once, nests, and rethrows") {
    std::vector<std::atomic<int>> hits(1000);
    parallelFor(1000, [&](int y) {
        // nested: the inner loop runs on the pool too and must not deadlock
        std::atomic<int> inner{0};
        parallelFor(40, [&](int) { ++inner; });
        hits[size_t(y)] += inner == 40 ? 1 : 100;
    });
    CHECK(std::all_of(hits.begin(), hits.end(), [](const std::atomic<int>& h) { return h == 1; }));

    CHECK_THROWS_AS(parallelFor(500, [](int y) {
                        if (y == 321) throw std::runtime_error("boom");
                    }),
                    std::runtime_error);
}

TEST_CASE("A raised cancel flag stops parallel work with EvalCancelled") {
    std::atomic<bool> cancel{false};
    std::atomic<int> done{0};
    parallel::CancelScope scope(&cancel);
    CHECK_THROWS_AS(parallelFor(100000, [&](int y) {
                        if (y == 10) cancel = true;
                        ++done;
                    }),
                    EvalCancelled);
    CHECK(done < 100000);
    cancel = false;
    parallelFor(100, [&](int) {});  // usable again once the flag drops
}

namespace {

// The previous implementation: one in-place clamped pass per radius and axis, column by column.
void referenceBlur(std::vector<float>& data, int w, int h, int ch, const std::vector<int>& rx, const std::vector<int>& ry) {
    auto pass = [&](int lines, int len, size_t lineStride, size_t elemStride, int radius) {
        if (radius <= 0) return;  // a running sum isn't an exact identity, so r = 0 is skipped
        for (int l = 0; l < lines; ++l) {
            std::vector<float> line(size_t(len) * ch);
            float* base = data.data() + size_t(l) * lineStride;
            for (int i = 0; i < len; ++i)
                for (int c = 0; c < ch; ++c) line[size_t(i) * ch + c] = base[size_t(i) * elemStride + c];
            const float inv = 1.0f / float(2 * radius + 1);
            for (int c = 0; c < ch; ++c) {
                auto at = [&](int i) { return line[size_t(std::clamp(i, 0, len - 1)) * ch + c]; };
                float acc = 0;
                for (int i = -radius; i <= radius; ++i) acc += at(i);
                for (int i = 0; i < len; ++i) {
                    base[size_t(i) * elemStride + c] = acc * inv;
                    acc += at(i + radius + 1) - at(i - radius);
                }
            }
        }
    };
    for (int r : rx) pass(h, w, size_t(w) * ch, size_t(ch), r);
    for (int r : ry) pass(w, h, size_t(ch), size_t(w) * ch, r);
}

}  // namespace

TEST_CASE("Blocked box blur is bit-identical to the per-column version") {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    const int w = 37, h = 29;  // not a multiple of the column block
    for (int axis = 0; axis < 3; ++axis)
    for (int ch : {1, 4}) {
        std::vector<float> a(size_t(w) * h * ch);
        for (float& v : a) v = d(rng);
        std::vector<float> b = a;
        // sigma 1.2 gives radii {0, 1, 1}: the zero pass must be skipped, as before
        const float sx = axis == 1 ? 0.0f : 3.0f, sy = axis == 0 ? 0.0f : 1.2f;
        CAPTURE(axis);
        if (ch == 1) {
            imageops::blurChannel(a, w, h, sx, sy);
        } else {
            Image img(w, h);
            img.px.assign(a.begin(), a.end());
            imageops::blurImage(img, sx, sy);
            a.assign(img.px.begin(), img.px.end());
        }
        // Mirrors boxRadii() in ImageOps.cpp.
        auto radii = [](float sigma) {
            std::vector<int> r;
            if (sigma < 0.3f) return r;
            float wIdeal = std::sqrt(12.0f * sigma * sigma / 3 + 1.0f);
            int wl = int(std::floor(wIdeal));
            if (wl % 2 == 0) --wl;
            int wu = wl + 2;
            float mIdeal = (12.0f * sigma * sigma - 3 * wl * wl - 4.0f * 3 * wl - 3.0f * 3) / (-4.0f * wl - 4.0f);
            int m = int(std::round(mIdeal));
            for (int i = 0; i < 3; ++i) r.push_back(((i < m ? wl : wu) - 1) / 2);
            return r;
        };
        referenceBlur(b, w, h, ch, radii(sx), radii(sy));
        float maxDiff = 0;
        int diffs = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) ++diffs, maxDiff = std::max(maxDiff, std::abs(a[i] - b[i]));
        CAPTURE(ch);
        CAPTURE(diffs);
        CAPTURE(maxDiff);
        CHECK(diffs == 0);
    }
}
