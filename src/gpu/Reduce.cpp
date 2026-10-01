#include "gpu/Reduce.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "gpu/GL.h"

namespace gpu {

namespace {

constexpr int kBins = 4096;     // 12-bit digits (16 KB of shared counters per work group)
constexpr int kGroups = 128;    // work groups per pass; each thread strides over the pixels

// Counts the pixels whose sort key (the float's bits, flipped so unsigned order is float order)
// matches uPrefix above bit uMatchShift, by the digit (key >> uShift) & uMask. Each group counts
// in shared memory first, so the global atomics are per bin and group rather than per pixel.
const char* const kHistogramSource = R"(#version 430
layout(local_size_x = 256) in;
layout(binding = 0) uniform sampler2D uSrc;
layout(std430, binding = 0) buffer Hist { uint hist[]; };
uniform ivec2 uSize;
uniform uint uPrefix, uMask;
uniform int uShift, uMatchShift;  // uMatchShift 32: every pixel matches
const uint BINS = 4096u;
shared uint counts[BINS];
void main() {
    uint li = gl_LocalInvocationIndex;
    for (uint i = li; i < BINS; i += 256u) counts[i] = 0u;
    barrier();
    uint n = uint(uSize.x * uSize.y), stride = gl_NumWorkGroups.x * 256u;
    for (uint i = gl_GlobalInvocationID.x; i < n; i += stride) {
        uint b = floatBitsToUint(texelFetch(uSrc, ivec2(int(i % uint(uSize.x)), int(i / uint(uSize.x))), 0).r);
        uint key = (b & 0x80000000u) != 0u ? ~b : (b | 0x80000000u);
        if (uMatchShift >= 32 || (key >> uint(uMatchShift)) == uPrefix) atomicAdd(counts[(key >> uint(uShift)) & uMask], 1u);
    }
    barrier();
    for (uint i = li; i < BINS; i += 256u)
        if (counts[i] != 0u) atomicAdd(hist[i], counts[i]);
}
)";

struct Buffer {
    unsigned id = 0;
    Buffer() { gl::GenBuffers(1, &id); }
    ~Buffer() { gl::DeleteBuffers(1, &id); }
};

}  // namespace

std::vector<float> select(const Texture& tex, const std::vector<size_t>& ranks) {
    const unsigned prog = program(kHistogramSource);
    Buffer buf;
    std::vector<uint32_t> hist(kBins);
    // One pass: the histogram of one digit among the keys starting with `prefix`.
    auto pass = [&](uint32_t prefix, int matchShift, int shift, uint32_t mask) {
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, buf.id);
        std::memset(hist.data(), 0, hist.size() * sizeof(uint32_t));
        gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(kBins * sizeof(uint32_t)), hist.data(), gl::DYNAMIC_READ);
        gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 0, buf.id);
        gl::UseProgram(prog);
        gl::Uniform2i(gl::GetUniformLocation(prog, "uSize"), tex.w(), tex.h());
        gl::Uniform1ui(gl::GetUniformLocation(prog, "uPrefix"), prefix);
        gl::Uniform1ui(gl::GetUniformLocation(prog, "uMask"), mask);
        gl::Uniform1i(gl::GetUniformLocation(prog, "uShift"), shift);
        gl::Uniform1i(gl::GetUniformLocation(prog, "uMatchShift"), matchShift);
        bindTexture(0, tex);
        dispatchGroups(kGroups, 1);
        gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0, gl::GLsizeiptr(kBins * sizeof(uint32_t)), hist.data());
        if (gl::GetError() != gl::NO_ERROR) throw Error("GPU: histogram failed");
    };
    // The digit holding rank k, and k's rank among the keys with that digit.
    auto find = [&](size_t& k) {
        for (uint32_t d = 0; d < uint32_t(kBins); ++d) {
            if (k < hist[d]) return d;
            k -= hist[d];
        }
        throw Error("GPU: rank beyond the image");
    };

    // The top digit is shared by all ranks, so its pass runs once.
    pass(0, 32, 20, 0xFFF);
    const std::vector<uint32_t> top = hist;
    std::vector<float> values;
    for (size_t k : ranks) {
        hist = top;
        const uint32_t d1 = find(k);
        pass(d1, 20, 8, 0xFFF);
        const uint32_t d2 = find(k);
        pass((d1 << 12) | d2, 8, 0, 0xFF);
        const uint32_t key = (d1 << 20) | (d2 << 8) | find(k);
        const uint32_t bits = (key & 0x80000000u) ? (key & 0x7FFFFFFFu) : ~key;
        float v;
        std::memcpy(&v, &bits, sizeof v);
        values.push_back(v);
    }
    return values;
}

}  // namespace gpu
