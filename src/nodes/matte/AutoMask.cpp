#include "nodes/matte/AutoMask.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <thread>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "io/Paths.h"
#include "ml/Models.h"
#include "ml/Onnx.h"
#include "nodes/ImageOps.h"

using namespace nodeutil;
using namespace colormath;

namespace {

// Model results are cached by the picture they were made from, not its exact pixels: the preview,
// the draft and the export each feed the model a slightly different copy of the same photo (a
// model run takes up to a minute on a laptop CPU), and a small edit upstream barely moves a
// subject or the sky. A picture is a kThumb x kThumb thumbnail of the model's input.
constexpr int kThumb = 32;
constexpr size_t kThumbSize = 3 * kThumb * kThumb;

struct Memo {
    std::string id;
    size_t hash;               // of the exact input
    std::vector<float> thumb;  // kThumbSize, 0..1
    std::vector<float> probs;
    int pw, ph;
};
std::mutex g_memoMutex;
std::deque<Memo> g_memo;  // most recent first

// One channel of the model's input: the image averaged down (or interpolated up) to mw x mh, as
// display-referred sRGB values, as the models were trained on photos.
void prepare(const Image& img, bool linear, int mw, int mh, const AutoMaskNode::Model& m, std::vector<float>& t) {
    const int w = img.w, h = img.h;
    const size_t plane = size_t(mw) * mh;
    t.assign(3 * plane, 0.0f);
    parallelFor(mh, [&](int y) {
        const float fy0 = float(y) * h / mh, fy1 = float(y + 1) * h / mh;
        const int y0 = std::clamp(int(fy0), 0, h - 1), y1 = std::clamp(std::max(int(std::ceil(fy1)), y0 + 1), 1, h);
        for (int x = 0; x < mw; ++x) {
            const float fx0 = float(x) * w / mw, fx1 = float(x + 1) * w / mw;
            const int x0 = std::clamp(int(fx0), 0, w - 1), x1 = std::clamp(std::max(int(std::ceil(fx1)), x0 + 1), 1, w);
            float sum[3] = {};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const float* p = img.pixel(size_t(sy) * w + sx);
                    for (int k = 0; k < 3; ++k) sum[k] += std::isfinite(p[k]) ? p[k] : 0.0f;
                }
            const float inv = 1.0f / float((y1 - y0) * (x1 - x0));
            for (int k = 0; k < 3; ++k) {
                float v = std::clamp(sum[k] * inv, 0.0f, 1.0f);
                if (linear) v = linearToSrgb(v);
                t[k * plane + size_t(y) * mw + x] = (v - m.mean[k]) / m.std[k];
            }
        }
    });
}

// The input's thumbnail, back in 0..1 sRGB.
std::vector<float> thumbnail(const std::vector<float>& t, int mw, int mh, const AutoMaskNode::Model& m) {
    std::vector<float> th(kThumbSize, 0.0f);
    const size_t plane = size_t(mw) * mh;
    for (int k = 0; k < 3; ++k)
        for (int ty = 0; ty < kThumb; ++ty)
            for (int tx = 0; tx < kThumb; ++tx) {
                const int y0 = ty * mh / kThumb, y1 = std::max((ty + 1) * mh / kThumb, y0 + 1);
                const int x0 = tx * mw / kThumb, x1 = std::max((tx + 1) * mw / kThumb, x0 + 1);
                double sum = 0;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) sum += t[k * plane + size_t(y) * mw + x];
                th[(k * kThumb + ty) * kThumb + tx] =
                    float(sum / ((y1 - y0) * (x1 - x0))) * m.std[k] + m.mean[k];
            }
    return th;
}

// The same picture: on average within about 1% and nowhere more than 6%. Different crops and
// different photos differ by far more; a resampled copy of the same one by far less.
bool samePicture(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != kThumbSize || b.size() != kThumbSize) return false;
    double sum = 0;
    for (size_t i = 0; i < kThumbSize; ++i) {
        const float d = std::fabs(a[i] - b[i]);
        if (!(d <= 0.06f)) return false;
        sum += d;
    }
    return sum / kThumbSize <= 0.012;
}

// How alike two pictures' shapes are: the correlation of each channel, averaged, so brightness,
// contrast and white balance don't count. 1 is the same picture; different photos, and different
// crops of one, score far lower.
double similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != kThumbSize || b.size() != kThumbSize) return 0.0;
    constexpr size_t plane = size_t(kThumb) * kThumb;
    double total = 0;
    for (size_t k = 0; k < 3; ++k) {
        double ma = 0, mb = 0;
        for (size_t i = 0; i < plane; ++i) ma += a[k * plane + i], mb += b[k * plane + i];
        ma /= plane, mb /= plane;
        double va = 0, vb = 0, cov = 0;
        for (size_t i = 0; i < plane; ++i) {
            const double da = a[k * plane + i] - ma, db = b[k * plane + i] - mb;
            va += da * da, vb += db * db, cov += da * db;
        }
        if (va < 1e-6 || vb < 1e-6) return 0.0;  // a flat channel says nothing
        total += cov / std::sqrt(va * vb);
    }
    return total / 3.0;
}

// The model's result for this picture: the same one, or the same one with its tones edited (an
// exposure or white balance change upstream doesn't move the subject or the sky, and re-running
// the model for it would take up to a minute).
constexpr double kSameShape = 0.98;
bool matches(const std::vector<float>& a, const std::vector<float>& b) {
    return samePicture(a, b) || similarity(a, b) >= kSameShape;
}
// Close enough to show while the model runs on the new picture (a crop or a retouch upstream).
constexpr double kSimilarShape = 0.8;

// The disk cache in the models folder, so reopening a project doesn't run the model again:
// <model>-<hash>.mask files, the oldest removed past kDiskEntries.
constexpr int kDiskEntries = 64;
constexpr char kMagic[8] = {'N', 'L', 'M', 'A', 'S', 'K', '1', 0};

std::filesystem::path cacheDir() { return u8ToPath(ml::folder()) / "cache"; }

bool readMask(const std::filesystem::path& file, Memo& e, bool thumbOnly) {
    std::ifstream f(file, std::ios::binary);
    char magic[8];
    int32_t dims[2];
    e.thumb.resize(kThumbSize);
    if (!f.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0 || !f.read(reinterpret_cast<char*>(dims), 8) ||
        !f.read(reinterpret_cast<char*>(e.thumb.data()), kThumbSize * sizeof(float)))
        return false;
    if (dims[0] <= 0 || dims[1] <= 0 || dims[0] > 8192 || dims[1] > 8192) return false;
    e.pw = dims[0], e.ph = dims[1];
    if (thumbOnly) return true;
    std::vector<uint16_t> q(size_t(e.pw) * e.ph);
    if (!f.read(reinterpret_cast<char*>(q.data()), q.size() * sizeof(uint16_t))) return false;
    e.probs.resize(q.size());
    for (size_t i = 0; i < q.size(); ++i) e.probs[i] = q[i] / 65535.0f;
    return true;
}

void writeMask(const Memo& e) {
    std::error_code ec;
    const std::filesystem::path dir = cacheDir();
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (e.id + "-" + std::to_string(e.hash) + ".mask");
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        const int32_t dims[2] = {e.pw, e.ph};
        std::vector<uint16_t> q(e.probs.size());
        for (size_t i = 0; i < q.size(); ++i) q[i] = uint16_t(std::lround(std::clamp(e.probs[i], 0.0f, 1.0f) * 65535.0f));
        f.write(kMagic, 8);
        f.write(reinterpret_cast<const char*>(dims), 8);
        f.write(reinterpret_cast<const char*>(e.thumb.data()), kThumbSize * sizeof(float));
        f.write(reinterpret_cast<const char*>(q.data()), q.size() * sizeof(uint16_t));
        if (!f) {
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    // Keep the newest kDiskEntries.
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec))
        if (de.path().extension() == ".mask") files.emplace_back(de.last_write_time(ec), de.path());
    if (int(files.size()) <= kDiskEntries) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = kDiskEntries; i < files.size(); ++i) std::filesystem::remove(files[i].second, ec);
}

// A cached result for this input: in memory, then on disk. Call with g_memoMutex held.
bool lookup(const std::string& id, size_t hash, const std::vector<float>& thumb, Memo& out) {
    for (size_t i = 0; i < g_memo.size(); ++i)
        if (g_memo[i].id == id && (g_memo[i].hash == hash || matches(g_memo[i].thumb, thumb))) {
            out = g_memo[i];
            return true;
        }
    std::error_code ec;
    const std::string prefix = id + "-";
    for (const auto& de : std::filesystem::directory_iterator(cacheDir(), ec)) {
        const std::string name = pathToU8(de.path().filename());
        if (de.path().extension() != ".mask" || name.rfind(prefix, 0) != 0) continue;
        Memo e;
        const bool exact = name == prefix + std::to_string(hash) + ".mask";
        if (!readMask(de.path(), e, true) || (!exact && !matches(e.thumb, thumb))) continue;
        if (!readMask(de.path(), e, false)) continue;
        std::filesystem::last_write_time(de.path(), std::filesystem::file_time_type::clock::now(), ec);
        e.id = id, e.hash = hash;
        out = e;
        g_memo.push_front(std::move(e));
        if (g_memo.size() > 4) g_memo.pop_back();
        return true;
    }
    return false;
}

// Why each model's last picture has no mask, and the pictures it failed on (so a failing run, such
// as one out of memory, isn't started again on every evaluation; installing or removing models
// gives them a new try). Under g_memoMutex.
std::map<std::string, std::string> g_errors;
struct Failure {
    std::string id;
    std::vector<float> thumb;
    std::string err;
    int generation;  // ml::generation() then
};
std::vector<Failure> g_failures;

// The model's output as a mask, stored in the caches. False with err set when it isn't one.
bool store(const AutoMaskNode::Model& m, size_t hash, std::vector<float> thumb, const ml::Tensor& output,
           std::string& err, std::vector<float>* probsOut, int* pwOut, int* phOut) {
    if (output.shape.size() < 2) {
        err = "Unexpected model output";
        return false;
    }
    const int ph = int(output.shape[output.shape.size() - 2]);
    const int pw = int(output.shape[output.shape.size() - 1]);
    if (pw <= 0 || ph <= 0 || output.data.size() < size_t(pw) * ph) {
        err = "Unexpected model output";
        return false;
    }
    std::vector<float> probs(output.data.begin(), output.data.begin() + size_t(pw) * ph);
    for (float& v : probs) {
        if (m.sigmoid) v = 1.0f / (1.0f + std::exp(-v));
        v = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    }
    if (probsOut) *probsOut = probs, *pwOut = pw, *phOut = ph;
    std::lock_guard<std::mutex> lock(g_memoMutex);
    g_memo.push_front({m.id, hash, std::move(thumb), std::move(probs), pw, ph});
    if (g_memo.size() > 4) g_memo.pop_back();
    writeMask(g_memo.front());
    return true;
}

void recordFailure(const std::string& id, const std::vector<float>& thumb, const std::string& err) {
    std::lock_guard<std::mutex> lock(g_memoMutex);
    g_failures.push_back({id, thumb, err, ml::generation()});
    if (g_failures.size() > 8) g_failures.erase(g_failures.begin());
}

// A recorded failure for this picture. Call with g_memoMutex held.
const Failure* failedOn(const std::string& id, const std::vector<float>& thumb) {
    for (const Failure& f : g_failures)
        if (f.id == id && f.generation == ml::generation() && matches(f.thumb, thumb)) return &f;
    return nullptr;
}

std::atomic<int> g_resultGeneration{1};

// Model runs for the interactive previews, one at a time on their own thread: a run takes up to a
// minute on a laptop CPU, and evaluations are cancelled and restarted on every edit, so a run
// inside one would start over until the editing stopped, and hold up everything after it.
class Runner {
public:
    struct Request {
        AutoMaskNode::Model m;
        size_t hash;
        std::vector<float> thumb;
        ml::Tensor input;
    };

    ~Runner() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cancel_ = true;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void request(Request r) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ && runningId_ == r.m.id && matches(runningThumb_, r.thumb)) return;
        // A newer picture for a model replaces the one waiting.
        bool queued = false;
        for (Request& q : queue_)
            if (std::string_view(q.m.id) == r.m.id) q = std::move(r), queued = true;
        if (!queued) queue_.push_back(std::move(r));
        if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
        cv_.notify_all();
    }

    AutoMaskNode::Progress progress() {
        std::lock_guard<std::mutex> lock(mutex_);
        AutoMaskNode::Progress p;
        p.running = running_;
        p.queued = int(queue_.size());
        if (!running_) return p;
        p.model = runningId_;
        p.loading = ml::stage() == ml::Stage::Loading;
        p.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        auto it = took_.find(runningId_);
        // First guesses from a 4-core laptop CPU: the model's load and run.
        p.expected = it != took_.end() ? it->second : runningId_ == "subject" ? 50.0 : 4.0;
        return p;
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return quit_ || (!running_ && queue_.empty()); });
    }

private:
    void loop() {
        parallel::lowerThreadPriority();  // the UI and the previews come first
        for (;;) {
            Request r;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
                if (quit_) return;
                r = std::move(queue_.front());
                queue_.pop_front();
                running_ = true;
                runningId_ = r.m.id;
                runningThumb_ = r.thumb;
                started_ = std::chrono::steady_clock::now();
            }
            ml::Tensor output;
            std::string err;
            bool ok = ml::run(r.m.id, r.input, output, err, &cancel_);
            r.input = {};
            if (cancel_) return;  // quitting
            if (ok) ok = store(r.m, r.hash, r.thumb, output, err, nullptr, nullptr, nullptr);
            output = {};
            if (!ok) recordFailure(r.m.id, r.thumb, err);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                if (ok) took_[runningId_] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
            }
            ++g_resultGeneration;
            idle_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_, idle_;
    std::thread thread_;
    std::deque<Request> queue_;
    bool quit_ = false;
    std::atomic<bool> cancel_{false};  // only when quitting
    bool running_ = false;
    std::string runningId_;
    std::vector<float> runningThumb_;
    std::chrono::steady_clock::time_point started_;
    std::map<std::string, double> took_;  // seconds the model's last run took
};

Runner& runner() {
    static Runner r;
    return r;
}

// He et al.'s guided filter: the mask p follows the edges of the guide I within about sigma.
std::vector<float> guidedFilter(const std::vector<float>& I, const std::vector<float>& p, int w, int h, float sigma,
                                float eps) {
    const size_t n = I.size();
    std::vector<float> mI = I, mP = p, mIP(n), mII(n);
    for (size_t i = 0; i < n; ++i) mIP[i] = I[i] * p[i], mII[i] = I[i] * I[i];
    for (auto* c : {&mI, &mP, &mIP, &mII}) imageops::blurChannel(*c, w, h, sigma, sigma);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        const float var = std::max(mII[i] - mI[i] * mI[i], 0.0f);
        a[i] = (mIP[i] - mI[i] * mP[i]) / (var + eps);
        b[i] = mP[i] - a[i] * mI[i];
    }
    imageops::blurChannel(a, w, h, sigma, sigma);
    imageops::blurChannel(b, w, h, sigma, sigma);
    for (size_t i = 0; i < n; ++i) a[i] = std::clamp(a[i] * I[i] + b[i], 0.0f, 1.0f);
    return a;
}

}  // namespace

AutoMaskNode::Inferred AutoMaskNode::infer(const Image& img, bool linear, bool background, std::vector<float>& probs,
                                          int& pw, int& ph, std::string& err,
                                          const std::atomic<bool>* cancel) const {
    const Model& m = model();
    auto result = [&](Inferred r) {
        std::lock_guard<std::mutex> lock(g_memoMutex);
        g_errors[m.id] = r == Inferred::Failed ? err : std::string();
        return r;
    };
    if (!ml::available(m.id)) {
        err = "The model isn't installed";
        return result(Inferred::Failed);
    }
    // The pinned models' input size, so a cached result doesn't wait seconds for a session.
    const int mw = m.width, mh = m.height;
    ml::Tensor input{{1, 3, mh, mw}, {}};
    prepare(img, linear, mw, mh, m, input.data);
    const size_t hash = std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(input.data.data()), input.data.size() * sizeof(float)));
    std::vector<float> thumb = thumbnail(input.data, mw, mh, m);
    {
        std::lock_guard<std::mutex> lock(g_memoMutex);
        Memo e;
        if (lookup(m.id, hash, thumb, e)) {
            probs = std::move(e.probs), pw = e.pw, ph = e.ph;
            g_errors[m.id].clear();
            return Inferred::Ready;
        }
        if (const Failure* f = failedOn(m.id, thumb)) {
            err = f->err;
            g_errors[m.id] = err;
            return Inferred::Failed;
        }
    }
    if (background) {
        // Meanwhile, the mask of the most similar recent picture (the same photo before a crop
        // or a retouch), or none.
        {
            std::lock_guard<std::mutex> lock(g_memoMutex);
            double best = kSimilarShape;
            for (const Memo& e : g_memo)
                if (e.id == m.id)
                    if (const double sim = similarity(e.thumb, thumb); sim >= best) {
                        best = sim;
                        probs = e.probs, pw = e.pw, ph = e.ph;
                    }
        }
        runner().request({m, hash, std::move(thumb), std::move(input)});
        return result(Inferred::Pending);
    }
    ml::Tensor output;
    if (!ml::run(m.id, input, output, err, cancel)) {
        if (cancel && cancel->load()) throw EvalCancelled();
        recordFailure(m.id, thumb, err);
        return result(Inferred::Failed);
    }
    input = {};
    if (!store(m, hash, std::move(thumb), output, err, &probs, &pw, &ph)) return result(Inferred::Failed);
    return result(Inferred::Ready);
}

AutoMaskNode::Progress AutoMaskNode::progress() { return runner().progress(); }
int AutoMaskNode::resultGeneration() { return g_resultGeneration.load(); }
void AutoMaskNode::waitForRuns() { runner().wait(); }

float AutoMaskNode::refineSigma(int fullW, int fullH, int pw, int ph) {
    // About one model pixel: edges are placed within the model's resolution, and the image's own
    // edges decide where inside it.
    const float px = std::max(float(fullW) / std::max(pw, 1), float(fullH) / std::max(ph, 1));
    return std::clamp(px * 0.75f, 1.0f, 64.0f);
}

int AutoMaskNode::roiPadding(const EvalContext& ctx) const {
    const std::vector<float>* s = ctx.previewStats;
    if (!s || s->size() < 2 || !ctx.roi) return kRoiWhole;
    const int pw = int((*s)[0]), ph = int((*s)[1]);
    if (!paramB(RefineEdges)) return 1;
    return 2 * imageops::blurReach(refineSigma(ctx.roi->canvasW, ctx.roi->canvasH, pw, ph)) + 2;
}

std::string AutoMaskNode::signatureExtra() const {
    return std::string(ml::available(model().id) ? "ready" : "missing") + (ml::useGpu() ? ",gpu" : ",cpu") + "," +
           std::to_string(resultGeneration());
}

std::string AutoMaskNode::lastError() const {
    std::lock_guard<std::mutex> lock(g_memoMutex);
    auto it = g_errors.find(model().id);
    return it != g_errors.end() ? it->second : std::string();
}

void AutoMaskNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    ImagePtr src = toImage(in[0], 0, 0);
    if (!src) return;
    const int w = src->w, h = src->h;
    const nodeutil::PixelFrame f = nodeutil::frameOf(ctx, w, h);

    // The low-resolution mask: the preview's for a region, otherwise the model's (whole image).
    std::vector<float> probs;
    int pw = 0, ph = 0;
    std::string err;
    const std::vector<float>* s = ctx.previewStats;
    if (ctx.roi && s && s->size() >= 2 && s->size() == 2 + size_t((*s)[0]) * size_t((*s)[1])) {
        pw = int((*s)[0]), ph = int((*s)[1]);
        probs.assign(s->begin() + 2, s->end());
    } else if (infer(*src, ctx.linear(), ctx.interactive, probs, pw, ph, err, ctx.cancel) == Inferred::Ready) {
        // Not for a stand-in while the model runs: regions then wait for the real mask too.
        if (ctx.statsOut) {
            ctx.statsOut->assign({float(pw), float(ph)});
            ctx.statsOut->insert(ctx.statsOut->end(), probs.begin(), probs.end());
        }
    }

    std::vector<float> m(size_t(w) * h, 0.0f);
    if (!probs.empty()) {
        const float sx = float(pw) / f.fullW, sy = float(ph) / f.fullH;
        parallelFor(h, [&](int y) {
            const float v = (f.y0 + y + 0.5f) * sy;
            for (int x = 0; x < w; ++x)
                m[size_t(y) * w + x] = imageops::sampleBilinear(probs, pw, ph, (f.x0 + x + 0.5f) * sx, v);
        });
        if (paramB(RefineEdges)) {
            // Guided by perceptual lightness, so shadows and highlights weigh alike.
            std::vector<float> guide(m.size());
            const bool linear = ctx.linear();
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    const float* p = src->pixel(size_t(y) * w + x);
                    float l = luminance(p[0], p[1], p[2]);
                    l = std::isfinite(l) ? std::clamp(l, 0.0f, 1.0f) : 0.0f;
                    guide[size_t(y) * w + x] = linear ? linearToSrgb(l) : l;
                }
            });
            m = guidedFilter(guide, m, w, h, refineSigma(f.fullW, f.fullH, pw, ph), 1e-3f);
        }
    }
    ChannelPtr base = toChannel(in[1]);
    ChannelSampler sb{base.get(), w, h};
    const bool inv = paramB(Invert);
    out[0] = Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
        float v = m[size_t(y) * w + x];
        if (inv) v = 1.0f - v;
        return base ? v * std::clamp(sb(x, y), 0.0f, 1.0f) : v;
    })));
}

// BiRefNet (rembg's export) at 1024 x 1024 with ImageNet normalisation; its output is logits.
const AutoMaskNode::Model& SelectSubjectNode::model() const {
    static const Model m{"subject", 1024, 1024, {0.485f, 0.456f, 0.406f}, {0.229f, 0.224f, 0.225f}, true};
    return m;
}

// The U²-Net sky model at 320 x 320 with ImageNet normalisation; its output is already 0..1.
const AutoMaskNode::Model& SelectSkyNode::model() const {
    static const Model m{"sky", 320, 320, {0.485f, 0.456f, 0.406f}, {0.229f, 0.224f, 0.225f}, false};
    return m;
}
