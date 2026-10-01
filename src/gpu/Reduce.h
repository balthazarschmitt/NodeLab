#pragma once
// Statistics over a whole texture on the device, read back as a few numbers instead of the image.
#include <cstddef>
#include <vector>

#include "gpu/Device.h"

namespace gpu {

// The values of the red channel at the given ranks in sorted order (rank k = what
// std::nth_element puts at position k), exactly: a radix select over the float bits in three
// histogram passes of 12, 12 and 8 bits, each read back as one small buffer. Needs a Scope;
// throws gpu::Error on device failures. Ranks must be below w * h.
std::vector<float> select(const Texture& tex, const std::vector<size_t>& ranks);

}  // namespace gpu
