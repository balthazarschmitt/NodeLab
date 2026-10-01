#pragma once
// Blur on the GPU: the CPU's three box passes per axis (imageops::blurImage), each pass a running
// sum along chunks of a line, so the cost doesn't grow with the radius.
#include "gpu/Device.h"

namespace gpu {

// src blurred with Gaussian sigmas in pixels, in src's format (src itself when both are tiny).
// Needs a Scope.
TexturePtr boxBlur(const TexturePtr& src, float sigmaX, float sigmaY);

}  // namespace gpu
