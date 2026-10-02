#pragma once
// The OpenGL 4.3+ entry points the GPU device uses (compute shaders, image load/store, immutable
// textures), loaded through GLFW once the device's context exists. They live in namespace gl so
// they never clash with ImGui's loader, which the UI's OpenGL 3.0 context uses.
#include <cstddef>
#include <cstdint>

#ifndef APIENTRY
#ifdef _WIN32
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif

namespace gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLuint64 = uint64_t;
using GLsync = struct __GLsync*;

constexpr GLenum TEXTURE_2D = 0x0DE1;
constexpr GLenum TEXTURE0 = 0x84C0;
constexpr GLenum TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, NEAREST = 0x2600, LINEAR = 0x2601;
constexpr GLenum TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, CLAMP_TO_EDGE = 0x812F;
constexpr GLenum RGBA = 0x1908, RED = 0x1903, FLOAT = 0x1406, HALF_FLOAT = 0x140B;
constexpr GLenum RGBA8 = 0x8058, UNSIGNED_BYTE = 0x1401;
constexpr GLenum RGBA32F = 0x8814, RGBA16F = 0x881A, R32F = 0x822E, R16F = 0x822D;
constexpr GLenum READ_ONLY = 0x88B8, WRITE_ONLY = 0x88B9;
constexpr GLenum COMPUTE_SHADER = 0x91B9;
constexpr GLenum COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum ALL_BARRIER_BITS = 0xFFFFFFFF;
constexpr GLenum PACK_ALIGNMENT = 0x0D05, UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum NO_ERROR = 0, OUT_OF_MEMORY = 0x0505;
constexpr GLenum VERSION = 0x1F02, RENDERER = 0x1F01, VENDOR = 0x1F00;
constexpr GLenum MAX_COMPUTE_WORK_GROUP_COUNT = 0x91BE;
constexpr GLenum TIME_ELAPSED = 0x88BF, QUERY_RESULT = 0x8866;
constexpr GLenum SHADER_STORAGE_BUFFER = 0x90D2, STATIC_DRAW = 0x88E4, DYNAMIC_READ = 0x88E9,
                  PIXEL_PACK_BUFFER = 0x88EB, STREAM_READ = 0x88E1;
constexpr GLbitfield MAP_READ_BIT = 0x0001;

#define NODELAB_GL_FUNCS(X)                                                                         \
    X(void, GenTextures, (GLsizei n, GLuint* t))                                                    \
    X(void, DeleteTextures, (GLsizei n, const GLuint* t))                                           \
    X(void, BindTexture, (GLenum target, GLuint t))                                                 \
    X(void, TexParameteri, (GLenum target, GLenum name, GLint v))                                   \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, GetTexImage, (GLenum, GLint, GLenum, GLenum, void*))                                    \
    X(void, PixelStorei, (GLenum, GLint))                                                           \
    X(GLenum, GetError, ())                                                                         \
    X(void, Finish, ())                                                                             \
    X(void, Flush, ())                                                                              \
    X(const unsigned char*, GetString, (GLenum))                                                    \
    X(void, GetIntegeri_v, (GLenum, GLuint, GLint*))                                                \
    X(void, ActiveTexture, (GLenum))                                                                \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))                              \
    X(GLuint, CreateShader, (GLenum))                                                               \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                    \
    X(void, CompileShader, (GLuint))                                                                \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                  \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                 \
    X(void, DeleteShader, (GLuint))                                                                 \
    X(GLuint, CreateProgram, ())                                                                    \
    X(void, AttachShader, (GLuint, GLuint))                                                         \
    X(void, LinkProgram, (GLuint))                                                                  \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                 \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                \
    X(void, DeleteProgram, (GLuint))                                                                \
    X(void, UseProgram, (GLuint))                                                                   \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                           \
    X(void, Uniform1i, (GLint, GLint))                                                              \
    X(void, Uniform2i, (GLint, GLint, GLint))                                                       \
    X(void, Uniform1f, (GLint, GLfloat))                                                            \
    X(void, Uniform1fv, (GLint, GLsizei, const GLfloat*))                                           \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                                           \
    X(void, DispatchCompute, (GLuint, GLuint, GLuint))                                              \
    X(void, MemoryBarrier, (GLbitfield))                                                            \
    X(void, BindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))            \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                         \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                                \
    X(void, BindBuffer, (GLenum, GLuint))                                                           \
    X(void, BindBufferBase, (GLenum, GLuint, GLuint))                                               \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                  \
    X(GLboolean, UnmapBuffer, (GLenum))                                                              \
    X(void*, MapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield))                             \
    X(void, Uniform1ui, (GLint, GLuint))                                                             \
    X(void, GetBufferSubData, (GLenum, GLintptr, GLsizeiptr, void*))                                 \
    X(void, GenQueries, (GLsizei, GLuint*))                                                          \
    X(void, BeginQuery, (GLenum, GLuint))                                                            \
    X(void, EndQuery, (GLenum))                                                                      \
    X(void, GetQueryObjectui64v, (GLuint, GLenum, GLuint64*))                                        \
    X(void, CopyImageSubData, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei))

#define NODELAB_GL_DECLARE(ret, name, args) \
    using PFN_##name = ret(APIENTRY*) args;  \
    extern PFN_##name name;
NODELAB_GL_FUNCS(NODELAB_GL_DECLARE)
#undef NODELAB_GL_DECLARE

// Loads every function above from the current context. False if any is missing.
bool load(const char** missing = nullptr);

}  // namespace gl
