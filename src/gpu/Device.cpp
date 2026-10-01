#include "gpu/Device.h"

#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <GLFW/glfw3.h>

#include "gpu/GL.h"

namespace gpu {
namespace {

GLFWwindow* g_window = nullptr;
std::string g_description = "GPU device not started";
std::recursive_mutex g_deviceMutex;
thread_local int t_depth = 0;
thread_local GLFWwindow* t_previous = nullptr;

// Freed textures, kept for reuse: allocating storage is slow, and evaluation keeps asking for
// the same few sizes. Texture destructors run on any thread, so this holds only ids.
std::mutex g_poolMutex;
std::vector<unsigned> g_queries;  // free timer queries (g_poolMutex)
thread_local bool t_timing = false;
std::map<std::tuple<int, int, int>, std::vector<unsigned>> g_pool;
size_t g_pooledBytes = 0;
std::atomic<size_t> g_inUseBytes{0};

std::unordered_map<std::string, unsigned> g_programs;

gl::GLenum internalFormat(Format f) {
    switch (f) {
        case Format::RGBA16F: return gl::RGBA16F;
        case Format::RGBA32F: return gl::RGBA32F;
        case Format::R32F: return gl::R32F;
    }
    return gl::RGBA32F;
}

void checkError(const char* what) {
    const gl::GLenum e = gl::GetError();
    if (e == gl::NO_ERROR) return;
    throw Error(std::string("GPU: ") + what + (e == gl::OUT_OF_MEMORY ? " (out of GPU memory)" : " failed") +
                             " [GL error 0x" + [&] {
                                 char b[16];
                                 std::snprintf(b, sizeof b, "%04X", e);
                                 return std::string(b);
                             }() + "]");
}

}  // namespace

size_t bytesPerPixel(Format f) {
    switch (f) {
        case Format::RGBA16F: return 8;
        case Format::RGBA32F: return 16;
        case Format::R32F: return 4;
    }
    return 16;
}

const char* glslFormat(Format f) {
    switch (f) {
        case Format::RGBA16F: return "rgba16f";
        case Format::RGBA32F: return "rgba32f";
        case Format::R32F: return "r32f";
    }
    return "rgba32f";
}

bool init(std::string* why) {
    if (g_window) return true;
    auto fail = [&](const std::string& reason) {
        g_description = "No GPU device: " + reason;
        if (why) *why = reason;
        return false;
    };
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* w = glfwCreateWindow(16, 16, "NodeLab GPU", nullptr, nullptr);
    // Later windows (ImGui's floating panels) must get the UI's usual context.
    glfwDefaultWindowHints();
    if (!w) return fail("the driver has no OpenGL 4.3 (compute shaders)");
    GLFWwindow* prev = glfwGetCurrentContext();
    glfwMakeContextCurrent(w);
    const char* missing = nullptr;
    const bool loaded = gl::load(&missing);
    std::string desc;
    if (loaded) {
        const char* renderer = reinterpret_cast<const char*>(gl::GetString(gl::RENDERER));
        const char* version = reinterpret_cast<const char*>(gl::GetString(gl::VERSION));
        desc = std::string(renderer ? renderer : "?") + ", OpenGL " + (version ? version : "?");
    }
    glfwMakeContextCurrent(prev);
    if (!loaded) {
        glfwDestroyWindow(w);
        return fail(std::string("missing ") + missing);
    }
    g_window = w;
    g_description = desc;
    return true;
}

void shutdown() {
    if (!g_window) return;
    {
        Scope s;
        trimPool(0);
        for (auto& [src, p] : g_programs) gl::DeleteProgram(p);
        g_programs.clear();
        std::lock_guard lock(g_poolMutex);
        g_queries.clear();  // deleted with the context
    }
    glfwDestroyWindow(g_window);
    g_window = nullptr;
    g_description = "GPU device stopped";
}

bool available() { return g_window != nullptr; }
std::string description() { return g_description; }

Scope::Scope() {
    if (!g_window) throw Error("GPU: no device");
    g_deviceMutex.lock();
    if (t_depth++ == 0) {
        // The UI thread has its own context current; it gets it back afterwards.
        t_previous = glfwGetCurrentContext();
        glfwMakeContextCurrent(g_window);
    }
}

Scope::~Scope() {
    if (--t_depth == 0) glfwMakeContextCurrent(t_previous);
    g_deviceMutex.unlock();
}

Texture::~Texture() {
    std::lock_guard lock(g_poolMutex);
    g_inUseBytes -= bytes();
    g_pool[{w_, h_, int(fmt_)}].push_back(id_);
    g_pooledBytes += bytes();
}

TexturePtr allocate(int w, int h, Format f) {
    if (w <= 0 || h <= 0) throw Error("GPU: empty texture");
    unsigned id = 0;
    {
        std::lock_guard lock(g_poolMutex);
        auto it = g_pool.find({w, h, int(f)});
        if (it != g_pool.end() && !it->second.empty()) {
            id = it->second.back();
            it->second.pop_back();
            g_pooledBytes -= size_t(w) * h * bytesPerPixel(f);
        }
    }
    if (!id) {
        gl::GenTextures(1, &id);
        gl::BindTexture(gl::TEXTURE_2D, id);
        gl::TexStorage2D(gl::TEXTURE_2D, 1, internalFormat(f), w, h);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
        // Creating a texture binds it to the active unit: callers allocate before binding inputs.
        gl::BindTexture(gl::TEXTURE_2D, 0);
        const gl::GLenum e = gl::GetError();
        if (e != gl::NO_ERROR) {
            gl::DeleteTextures(1, &id);
            // Pooled textures may be what's using the memory: free them and try once more.
            if (e == gl::OUT_OF_MEMORY && bytesPooled() > 0) {
                trimPool(0);
                return allocate(w, h, f);
            }
            throw Error(e == gl::OUT_OF_MEMORY ? "GPU: out of GPU memory" : "GPU: texture allocation failed");
        }
    }
    auto t = std::make_shared<Texture>(id, w, h, f);
    g_inUseBytes += t->bytes();
    return t;
}

GpuImagePtr upload(const Image& img, Format f) {
    auto out = std::make_shared<GpuImage>();
    out->tex = allocate(img.w, img.h, f);
    out->w = img.w;
    out->h = img.h;
    gl::BindTexture(gl::TEXTURE_2D, out->tex->id());
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
    gl::TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, img.w, img.h, gl::RGBA, gl::FLOAT, img.px.data());
    checkError("upload");
    return out;
}

GpuChannelPtr upload(const Channel& c) {
    if (c.constant) throw Error("GPU: constant channels stay on the CPU");
    auto out = std::make_shared<GpuChannel>();
    out->tex = allocate(c.w, c.h, Format::R32F);
    out->w = c.w;
    out->h = c.h;
    gl::BindTexture(gl::TEXTURE_2D, out->tex->id());
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 4);
    gl::TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, c.w, c.h, gl::RED, gl::FLOAT, c.data.data());
    checkError("upload");
    return out;
}

ImagePtr download(const GpuImage& img) {
    Scope s;
    auto out = std::make_shared<Image>(img.w, img.h);
    gl::BindTexture(gl::TEXTURE_2D, img.tex->id());
    gl::PixelStorei(gl::PACK_ALIGNMENT, 4);
    gl::GetTexImage(gl::TEXTURE_2D, 0, gl::RGBA, gl::FLOAT, out->px.data());
    checkError("download");
    return out;
}

ChannelPtr download(const GpuChannel& c) {
    Scope s;
    auto out = std::make_shared<Channel>(Channel::makeSized(c.w, c.h));
    gl::BindTexture(gl::TEXTURE_2D, c.tex->id());
    gl::PixelStorei(gl::PACK_ALIGNMENT, 4);
    gl::GetTexImage(gl::TEXTURE_2D, 0, gl::RED, gl::FLOAT, out->data.data());
    checkError("download");
    return out;
}

Value toGpu(const Value& v, bool half) {
    if (auto p = std::get_if<ImagePtr>(&v.v); p && *p) return Value(upload(**p, half ? Format::RGBA16F : Format::RGBA32F));
    if (auto p = std::get_if<ChannelPtr>(&v.v); p && *p && !(*p)->constant) return Value(upload(**p));
    return v;
}

size_t valueBytes(const Value& v) {
    if (auto p = std::get_if<GpuImagePtr>(&v.v); p && *p && (*p)->tex) return (*p)->tex->bytes();
    if (auto p = std::get_if<GpuChannelPtr>(&v.v); p && *p && (*p)->tex) return (*p)->tex->bytes();
    return 0;
}

Timer::Timer() {
    if (t_timing) return;
    {
        std::lock_guard lock(g_poolMutex);
        if (!g_queries.empty()) {
            id_ = g_queries.back();
            g_queries.pop_back();
        }
    }
    if (!id_) gl::GenQueries(1, &id_);
    gl::BeginQuery(gl::TIME_ELAPSED, id_);
    running_ = t_timing = true;
}

Timer::~Timer() {
    stop();  // abandoned by an exception, still inside the Scope it started in
    if (!id_) return;
    std::lock_guard lock(g_poolMutex);
    g_queries.push_back(id_);
}

void Timer::stop() {
    if (!running_) return;
    gl::EndQuery(gl::TIME_ELAPSED);
    running_ = t_timing = false;
}

double Timer::ms() {
    if (!id_ || running_) return -1;
    if (ms_ < 0) {
        Scope device;
        gl::GLuint64 ns = 0;
        gl::GetQueryObjectui64v(id_, gl::QUERY_RESULT, &ns);
        ms_ = double(ns) * 1e-6;
    }
    return ms_;
}

void finish() {
    gl::Finish();
    checkError("evaluation");
}

unsigned program(const std::string& source) {
    if (auto it = g_programs.find(source); it != g_programs.end()) return it->second;
    const unsigned sh = gl::CreateShader(gl::COMPUTE_SHADER);
    const char* src = source.c_str();
    gl::ShaderSource(sh, 1, &src, nullptr);
    gl::CompileShader(sh);
    int ok = 0;
    gl::GetShaderiv(sh, gl::COMPILE_STATUS, &ok);
    auto log = [](auto getLen, auto getLog, unsigned obj) {
        int len = 0;
        getLen(obj, gl::INFO_LOG_LENGTH, &len);
        std::string s(size_t(std::max(len, 1)), '\0');
        getLog(obj, len, nullptr, s.data());
        return s;
    };
    if (!ok) {
        const std::string msg = log(gl::GetShaderiv, gl::GetShaderInfoLog, sh);
        gl::DeleteShader(sh);
        throw Error("GPU: shader compile failed: " + msg.substr(0, 600));
    }
    const unsigned prog = gl::CreateProgram();
    gl::AttachShader(prog, sh);
    gl::LinkProgram(prog);
    gl::DeleteShader(sh);
    gl::GetProgramiv(prog, gl::LINK_STATUS, &ok);
    if (!ok) {
        const std::string msg = log(gl::GetProgramiv, gl::GetProgramInfoLog, prog);
        gl::DeleteProgram(prog);
        throw Error("GPU: shader link failed: " + msg.substr(0, 600));
    }
    g_programs.emplace(source, prog);
    return prog;
}

void bindTexture(int unit, const Texture& t) {
    gl::ActiveTexture(gl::TEXTURE0 + unsigned(unit));
    gl::BindTexture(gl::TEXTURE_2D, t.id());
    gl::ActiveTexture(gl::TEXTURE0);
}

void bindImage(int unit, const Texture& t) {
    gl::BindImageTexture(unsigned(unit), t.id(), 0, 0, 0, gl::WRITE_ONLY, internalFormat(t.format()));
}

void dispatchGroups(unsigned x, unsigned y) {
    gl::DispatchCompute(x, y, 1);
    gl::MemoryBarrier(gl::ALL_BARRIER_BITS);
}

void dispatch(int w, int h) {
    gl::DispatchCompute(unsigned(w + 15) / 16, unsigned(h + 15) / 16, 1);
    gl::MemoryBarrier(gl::ALL_BARRIER_BITS);
}

size_t bytesInUse() { return g_inUseBytes.load(); }

size_t bytesPooled() {
    std::lock_guard lock(g_poolMutex);
    return g_pooledBytes;
}

void trimPool(size_t keep) {
    std::vector<unsigned> doomed;
    {
        std::lock_guard lock(g_poolMutex);
        for (auto it = g_pool.begin(); it != g_pool.end() && g_pooledBytes > keep;) {
            const auto& [w, h, f] = it->first;
            const size_t b = size_t(w) * h * bytesPerPixel(Format(f));
            while (!it->second.empty() && g_pooledBytes > keep) {
                doomed.push_back(it->second.back());
                it->second.pop_back();
                g_pooledBytes -= b;
            }
            it = it->second.empty() ? g_pool.erase(it) : std::next(it);
        }
    }
    if (!doomed.empty()) gl::DeleteTextures(int(doomed.size()), doomed.data());
}

}  // namespace gpu
