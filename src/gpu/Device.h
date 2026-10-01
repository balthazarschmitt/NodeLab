#pragma once
// GPU compositing device, like Blender's compositor Device: GPU. An OpenGL 4.3+ core context of
// its own (on a hidden window) runs compute shaders on images kept resident as textures.
//
// Copies between CPU and GPU cost more than most nodes do on the GPU (a 2.7 MP RGBA image takes
// 6-40 ms each way on an iGPU, a per-pixel node 2-3 ms), so the evaluator keeps values on the GPU
// between GPU nodes and only downloads them for CPU nodes and the viewer.
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Value.h"

namespace gpu {

// A GPU failure (out of memory, a shader the driver rejects): the evaluator runs the node on the
// CPU instead.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Creates the context. Call once on the main thread (GLFW creates windows only there), after
// glfwInit. False, with the reason, when the driver has no OpenGL 4.3 (the CPU then does
// everything, as before).
bool init(std::string* why = nullptr);
// Destroys the context (before glfwTerminate).
void shutdown();
bool available();
// "Intel(R) Iris(R) Plus Graphics, OpenGL 4.6.0 - Build ..." or why there is no GPU device.
std::string description();

// Makes the context current on this thread while it lives. The device serves one thread at a
// time; nested scopes on the same thread are free.
class Scope {
public:
    Scope();
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// How values are stored, like Blender's compositor Precision: Half (Auto) halves the memory
// traffic, which is what per-pixel nodes are limited by; Full matches the CPU's floats.
enum class Format { RGBA16F, RGBA32F, R32F };
size_t bytesPerPixel(Format f);
// The GLSL image format qualifier (rgba16f...).
const char* glslFormat(Format f);

// A texture from the device's pool. Destroying it returns it to the pool, from any thread.
class Texture {
public:
    Texture(unsigned id, int w, int h, Format f) : id_(id), w_(w), h_(h), fmt_(f) {}
    ~Texture();
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    unsigned id() const { return id_; }
    int w() const { return w_; }
    int h() const { return h_; }
    Format format() const { return fmt_; }
    size_t bytes() const { return size_t(w_) * h_ * bytesPerPixel(fmt_); }

private:
    unsigned id_;
    int w_, h_;
    Format fmt_;
};
using TexturePtr = std::shared_ptr<Texture>;

// These need a Scope. They throw gpu::Error on GL errors (out of memory...).
TexturePtr allocate(int w, int h, Format f);
GpuImagePtr upload(const Image& img, Format f);
GpuChannelPtr upload(const Channel& c);  // sized channels only
ImagePtr download(const GpuImage& img);
ChannelPtr download(const GpuChannel& c);
// A compute program compiled from `source` (a full shader), cached by its text. Throws with the
// compiler's log on errors.
unsigned program(const std::string& source);
// The value on the device: images and sized channels uploaded (images as RGBA16F when half,
// channels always R32F), numbers and constant channels unchanged.
Value toGpu(const Value& v, bool half);
// Bytes a GPU value's texture holds (0 for CPU values).
size_t valueBytes(const Value& v);
// Waits for the device to finish the work issued so far (glFinish).
void finish();
// Binds t for texelFetch at sampler `unit`, or for imageStore at image `unit` (layout bindings).
void bindTexture(int unit, const Texture& t);
void bindImage(int unit, const Texture& t);
// Runs the bound program in x by y work groups, then makes its writes visible.
void dispatchGroups(unsigned x, unsigned y);
// Runs the bound program over w x h pixels in 16 x 16 groups, then makes its writes visible.
void dispatch(int w, int h);

// The device time of the work issued while it runs (a GL timer query), measured without waiting
// for that work: the evaluator times GPU nodes with it instead of stalling after each one.
class Timer {
public:
    Timer();  // starts (needs a Scope); inactive while another timer runs, as queries can't nest
    ~Timer();  // from any thread
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    void stop();  // needs a Scope
    // Milliseconds, waiting for the work if it is still running (takes a Scope); -1 if inactive.
    double ms();

private:
    unsigned id_ = 0;
    bool running_ = false;
    double ms_ = -1;
};

// Memory held by textures in use and by the pool's free ones.
size_t bytesInUse();
size_t bytesPooled();
// Frees pooled textures beyond `keep` bytes (needs a Scope).
void trimPool(size_t keep);

}  // namespace gpu
