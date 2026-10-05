#include "ml/Onnx.h"
#include <chrono>
#include <set>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include "io/Paths.h"
#include "ml/Models.h"

#ifdef _WIN32
// Before windows.h: MinGW's sal.h then defines the annotations ONNX Runtime left empty.
#include "onnxruntime_c_api.h"
#include <windows.h>
#endif

namespace ml {

namespace {

// Off by default: on integrated GPUs DirectML takes minutes to set up a model and can run out
// of memory, while the CPU sets it up in seconds.
std::atomic<bool> g_useGpu{false};
std::atomic<bool> g_ranOnGpu{false};
std::atomic<int> g_stage{0};  // Stage

// Sets the stage for the length of a run.
struct StageScope {
    ~StageScope() { g_stage = int(Stage::Idle); }
};

}  // namespace

void setUseGpu(bool on) {
    if (g_useGpu.exchange(on) != on) releaseSessions();
}
bool useGpu() { return g_useGpu; }
bool ranOnGpu() { return g_ranOnGpu; }
Stage stage() { return Stage(g_stage.load()); }

#ifdef _WIN32

namespace {

// The start of OrtDmlApi (dml_provider_factory.h), whose full header needs the Direct3D 12 SDK.
struct DmlApiStart {
    OrtStatus*(ORT_API_CALL* SessionOptionsAppendExecutionProvider_DML)(OrtSessionOptions* options, int device_id);
};

struct Runtime {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    std::string error;  // why it couldn't load
};

struct Session {
    OrtSession* session = nullptr;
    std::string input, output;
    std::vector<int64_t> inputShape;
    bool gpu = false;
    int users = 0;   // runs using or waiting for it, under g_mutex
    std::mutex run;  // DirectML sessions take one run at a time
};

std::mutex g_mutex;  // the runtime and the session map
Runtime g_rt;
std::map<std::string, std::shared_ptr<Session>> g_sessions;
std::set<std::string> g_gpuFailed;  // models whose GPU run failed: the CPU runs them from then on

std::string statusText(const OrtApi* api, OrtStatus* st) {
    std::string s = api->GetErrorMessage(st);
    api->ReleaseStatus(st);
    return s;
}

// Loaded once, on first use, and kept: unloading ONNX Runtime safely isn't supported.
// Call with g_mutex held.
Runtime& runtime() {
    Runtime& rt = g_rt;
    static int triedGeneration = 0;
    if (rt.api || triedGeneration == generation()) return rt;
    triedGeneration = generation();
    rt.error.clear();
    const ModelSpec* spec = findModel(kRuntime);
    if (!installed(kRuntime)) {
        rt.error = "The AI runtime isn't installed";
        return rt;
    }
    // DirectML.dll first, by full path: ONNX Runtime then gets this copy rather than the older
    // one in System32, which it can't use.
    for (const FileSpec& f : spec->files)
        if (std::string(f.name) == "DirectML.dll") LoadLibraryExW(u8ToPath(filePath(f)).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE ort = nullptr;
    for (const FileSpec& f : spec->files)
        if (std::string(f.name) == "onnxruntime.dll")
            ort = LoadLibraryExW(u8ToPath(filePath(f)).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ort) {
        rt.error = "Couldn't load onnxruntime.dll (error " + std::to_string(GetLastError()) + ")";
        return rt;
    }
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    auto getApiBase = reinterpret_cast<GetApiBase>(reinterpret_cast<void*>(GetProcAddress(ort, "OrtGetApiBase")));
    const OrtApi* api = getApiBase ? getApiBase()->GetApi(ORT_API_VERSION) : nullptr;
    if (!api) {
        rt.error = "onnxruntime.dll is the wrong version";
        return rt;
    }
    if (OrtStatus* st = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "NodeLab", &rt.env)) {
        rt.error = statusText(api, st);
        return rt;
    }
    rt.api = api;
    return rt;
}

// The model's session, created on first use. Call with g_mutex held.
std::shared_ptr<Session> session(const std::string& id, std::string& err) {
    if (auto it = g_sessions.find(id); it != g_sessions.end()) return it->second;
    Runtime& rt = runtime();
    if (!rt.api) {
        err = rt.error;
        return nullptr;
    }
    const OrtApi* api = rt.api;
    const ModelSpec* spec = findModel(id);
    if (!spec || !installed(id)) {
        err = std::string(spec ? spec->title : "The model") + " isn't installed";
        return nullptr;
    }
    const std::wstring path = u8ToPath(filePath(spec->files[0])).wstring();
    auto s = std::make_shared<Session>();
    // GPU first (when wanted), then the CPU if DirectML can't take the model.
    for (int attempt = g_useGpu && !g_gpuFailed.count(id) ? 0 : 1; attempt < 2 && !s->session; ++attempt) {
        OrtSessionOptions* so = nullptr;
        if (OrtStatus* st = api->CreateSessionOptions(&so)) {
            err = statusText(api, st);
            return nullptr;
        }
        api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
        // Idle workers sleep rather than spin, so they don't slow the nodes evaluated after a run.
        api->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", "0");
        // Without the arena, a run's buffers go back to the system as it ends rather than staying
        // reserved at their peak (BiRefNet's is about 4 GB); it measured no slower.
        api->DisableCpuMemArena(so);
        bool ok = true;
        if (attempt == 0) {
            // DirectML needs sequential execution without memory patterns.
            api->DisableMemPattern(so);
            api->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
            const void* p = nullptr;
            OrtStatus* st = api->GetExecutionProviderApi("DML", ORT_API_VERSION, &p);
            if (!st && p) st = static_cast<const DmlApiStart*>(p)->SessionOptionsAppendExecutionProvider_DML(so, 0);
            if (st) {
                err = statusText(api, st);
                ok = false;
            }
        }
        if (ok) {
            if (OrtStatus* st = api->CreateSession(rt.env, path.c_str(), so, &s->session)) {
                err = statusText(api, st);
                s->session = nullptr;
            } else {
                s->gpu = attempt == 0;
                g_ranOnGpu = s->gpu;
            }
        }
        api->ReleaseSessionOptions(so);
        // DirectML couldn't take it: later loads go straight to the CPU.
        if (attempt == 0 && !s->session) g_gpuFailed.insert(id);
    }
    if (!s->session) return nullptr;
    err.clear();
    OrtAllocator* alloc = nullptr;
    api->GetAllocatorWithDefaultOptions(&alloc);
    char* name = nullptr;
    if (!api->SessionGetInputName(s->session, 0, alloc, &name)) {
        s->input = name;
        api->AllocatorFree(alloc, name);
    }
    if (!api->SessionGetOutputName(s->session, 0, alloc, &name)) {
        s->output = name;
        api->AllocatorFree(alloc, name);
    }
    OrtTypeInfo* ti = nullptr;
    if (!api->SessionGetInputTypeInfo(s->session, 0, &ti)) {
        const OrtTensorTypeAndShapeInfo* tsi = nullptr;
        size_t n = 0;
        if (!api->CastTypeInfoToTensorInfo(ti, &tsi) && tsi && !api->GetDimensionsCount(tsi, &n)) {
            s->inputShape.resize(n);
            api->GetDimensions(tsi, s->inputShape.data(), n);
        }
        api->ReleaseTypeInfo(ti);
    }
    g_sessions[id] = s;
    return s;
}

}  // namespace

std::vector<int64_t> inputShape(const std::string& modelId, std::string& err) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto s = session(modelId, err);
    return s ? s->inputShape : std::vector<int64_t>{};
}

namespace {

// CPU sessions are unloaded when their last run ends: a model's weights and working memory are
// hundreds of MB to GBs, results are cached by picture so the same model rarely runs twice in a
// row, and loading it again takes seconds. GPU sessions are kept, as DirectML takes far longer
// to set one up.
struct SessionUse {
    std::shared_ptr<Session> s;
    std::string id;
    ~SessionUse() {
        if (!s) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (--s->users > 0 || s->gpu) return;
        if (auto it = g_sessions.find(id); it != g_sessions.end() && it->second == s) g_sessions.erase(it);
        std::lock_guard<std::mutex> runLock(s->run);
        if (g_rt.api && s->session) g_rt.api->ReleaseSession(s->session);
        s->session = nullptr;
    }
};

bool runOnce(const std::string& modelId, const Tensor& input, Tensor& output, std::string& err,
             const std::atomic<bool>* cancel, bool& gpuFailed) {
    SessionUse use{nullptr, modelId};
    const OrtApi* api = nullptr;
    StageScope stageScope;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stage = int(g_sessions.count(modelId) ? Stage::Running : Stage::Loading);
        use.s = session(modelId, err);
        if (use.s) ++use.s->users;
        api = runtime().api;
    }
    Session* s = use.s.get();
    if (!s) return false;
    std::lock_guard<std::mutex> runLock(s->run);
    g_stage = int(Stage::Running);
    if (!s->session) {  // released (device changed, model removed) since it was looked up
        err = "The model was unloaded";
        return false;
    }
    OrtMemoryInfo* mem = nullptr;
    if (OrtStatus* st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem)) {
        err = statusText(api, st);
        return false;
    }
    OrtValue* in = nullptr;
    OrtStatus* st = api->CreateTensorWithDataAsOrtValue(mem, const_cast<float*>(input.data.data()),
                                                        input.data.size() * sizeof(float), input.shape.data(),
                                                        input.shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
    api->ReleaseMemoryInfo(mem);
    if (st) {
        err = statusText(api, st);
        return false;
    }
    OrtRunOptions* ro = nullptr;
    api->CreateRunOptions(&ro);
    // A run takes seconds on the CPU: a watcher stops it when the evaluation is cancelled.
    std::atomic<bool> done{false};
    std::thread watcher;
    if (cancel && ro)
        watcher = std::thread([&] {
            while (!done) {
                if (cancel->load()) {
                    api->RunOptionsSetTerminate(ro);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    const char* inName = s->input.c_str();
    const char* outName = s->output.c_str();
    OrtValue* out = nullptr;
    st = api->Run(s->session, ro, &inName, &in, 1, &outName, 1, &out);
    done = true;
    if (watcher.joinable()) watcher.join();
    if (ro) api->ReleaseRunOptions(ro);
    api->ReleaseValue(in);
    if (st) {
        const bool cancelled = cancel && cancel->load();
        err = cancelled ? "Cancelled" : statusText(api, st);
        if (cancelled) api->ReleaseStatus(st);
        gpuFailed = s->gpu && !cancelled;
        return false;
    }
    OrtTensorTypeAndShapeInfo* info = nullptr;
    size_t dims = 0, count = 0;
    float* data = nullptr;
    bool ok = !api->GetTensorTypeAndShape(out, &info) && !api->GetDimensionsCount(info, &dims);
    if (ok) {
        output.shape.resize(dims);
        ok = !api->GetDimensions(info, output.shape.data(), dims) && !api->GetTensorShapeElementCount(info, &count) &&
             !api->GetTensorMutableData(out, reinterpret_cast<void**>(&data));
    }
    if (info) api->ReleaseTensorTypeAndShapeInfo(info);
    if (ok && data) output.data.assign(data, data + count);
    api->ReleaseValue(out);
    if (!ok) err = "The model's output couldn't be read";
    return ok;
}

}  // namespace

bool run(const std::string& modelId, const Tensor& input, Tensor& output, std::string& err,
         const std::atomic<bool>* cancel) {
    bool gpuFailed = false;
    if (runOnce(modelId, input, output, err, cancel, gpuFailed)) return true;
    if (!gpuFailed) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_gpuFailed.insert(modelId);
        if (auto it = g_sessions.find(modelId); it != g_sessions.end()) {
            std::lock_guard<std::mutex> runLock(it->second->run);
            if (g_rt.api && it->second->session) g_rt.api->ReleaseSession(it->second->session);
            it->second->session = nullptr;
            g_sessions.erase(it);
        }
    }
    err.clear();
    return runOnce(modelId, input, output, err, cancel, gpuFailed);
}

void releaseSessions() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Runtime& rt = g_rt;
    for (auto& [id, s] : g_sessions) {
        std::lock_guard<std::mutex> runLock(s->run);
        if (rt.api && s->session) rt.api->ReleaseSession(s->session);
        s->session = nullptr;
    }
    g_sessions.clear();
    g_gpuFailed.clear();  // a new choice of device gets a new try
}

#else

std::vector<int64_t> inputShape(const std::string&, std::string& err) {
    err = "AI models need Windows";
    return {};
}
bool run(const std::string&, const Tensor&, Tensor&, std::string& err, const std::atomic<bool>*) {
    err = "AI models need Windows";
    return false;
}
void releaseSessions() {}

#endif

}  // namespace ml
