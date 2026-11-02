// Deko3D-only renderer factory. The optional Vulkan/NVK build provides this in vulkan.cpp instead.
#include "gpu/backend.h"

namespace gpu {

std::unique_ptr<Backend> CreateDekoBackend();

std::unique_ptr<Backend> CreateBackend() { return CreateDekoBackend(); }

}  // namespace gpu
