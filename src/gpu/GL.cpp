#include "gpu/GL.h"

#include <GLFW/glfw3.h>

namespace gl {

#define REFRACTORY_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
REFRACTORY_GL_FUNCS(REFRACTORY_GL_DEFINE)
#undef REFRACTORY_GL_DEFINE

bool load(const char** missing) {
    // OpenGL 1.1 functions are exported by opengl32.dll, not returned by wglGetProcAddress;
    // GLFW's lookup tries both.
#define REFRACTORY_GL_LOAD(ret, name, args)                                                \
    name = reinterpret_cast<PFN_##name>(glfwGetProcAddress("gl" #name));                 \
    if (!name) {                                                                         \
        if (missing) *missing = "gl" #name;                                              \
        return false;                                                                    \
    }
    REFRACTORY_GL_FUNCS(REFRACTORY_GL_LOAD)
#undef REFRACTORY_GL_LOAD
    return true;
}

}  // namespace gl
