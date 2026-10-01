#include "gpu/Blur.h"

#include <string>

#include "gpu/GL.h"
#include "nodes/ImageOps.h"

namespace gpu {

namespace {

constexpr int kChunk = 16;  // pixels per thread: its window sum is set up once per chunk

// One box pass along an axis (0: rows, 1: columns): a thread per line chunk, 64 lines per group,
// so neighbouring threads read neighbouring lines.
std::string passSource(int axis, Format f) {
    return std::string("#version 430\nlayout(local_size_x = 64) in;\n") +
           "layout(binding = 0) uniform sampler2D uSrc;\n"
           "layout(" + glslFormat(f) + ", binding = 0) uniform writeonly image2D uDst;\n"
           "uniform ivec2 uSize;\nuniform int uRadius;\n"
           "const int AXIS = " + std::to_string(axis) + ", CHUNK = " + std::to_string(kChunk) + ";\n" + R"(
int len() { return AXIS == 0 ? uSize.x : uSize.y; }
ivec2 pos(int line, int i) { return AXIS == 0 ? ivec2(i, line) : ivec2(line, i); }
// Edge-clamped, as the CPU's boxLine.
vec4 at(int line, int i) { return texelFetch(uSrc, pos(line, clamp(i, 0, len() - 1)), 0); }
void main() {
    int line = int(gl_GlobalInvocationID.x), i0 = int(gl_GlobalInvocationID.y) * CHUNK;
    if (line >= (AXIS == 0 ? uSize.y : uSize.x) || i0 >= len()) return;
    float inv = 1.0 / float(2 * uRadius + 1);
    vec4 acc = vec4(0.0);
    for (int i = i0 - uRadius; i <= i0 + uRadius; ++i) acc += at(line, i);
    int i1 = min(i0 + CHUNK, len());
    for (int i = i0; i < i1; ++i) {
        imageStore(uDst, pos(line, i), acc * inv);
        acc += at(line, i + uRadius + 1) - at(line, i - uRadius);
    }
}
)";
}

}  // namespace

TexturePtr boxBlur(const TexturePtr& src, float sigmaX, float sigmaY) {
    TexturePtr cur = src;
    const int w = src->w(), h = src->h();
    auto pass = [&](int axis, int radius) {
        if (radius <= 0) return;
        TexturePtr dst = allocate(w, h, src->format());  // before binding: creating binds it
        const unsigned prog = program(passSource(axis, src->format()));
        gl::UseProgram(prog);
        gl::Uniform2i(gl::GetUniformLocation(prog, "uSize"), w, h);
        gl::Uniform1i(gl::GetUniformLocation(prog, "uRadius"), radius);
        bindTexture(0, *cur);
        bindImage(0, *dst);
        const int lines = axis == 0 ? h : w, len = axis == 0 ? w : h;
        dispatchGroups(unsigned(lines + 63) / 64, unsigned(len + kChunk - 1) / kChunk);
        cur = dst;
    };
    for (int r : imageops::boxRadii(sigmaX)) pass(0, r);
    for (int r : imageops::boxRadii(sigmaY)) pass(1, r);
    if (gl::GetError() != gl::NO_ERROR) throw Error("GPU: blur failed");
    return cur;
}

}  // namespace gpu
