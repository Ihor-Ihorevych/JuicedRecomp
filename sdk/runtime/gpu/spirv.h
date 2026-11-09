#pragma once
// HLSL (the text shaders.cpp generates) to SPIR-V for the Vulkan backend, with glslang's HLSL front end.
//
// Resource layout every shader shares, descriptor set 0:
//   binding 0   cbuffer b0  the vertex constants
//   binding 1   cbuffer b1  DrawConstants (or a pass's own)
//   binding 2-5 t0-t3       sampled images
//   binding 6-9 s0-s3       samplers
// Vertex inputs: ATTRn is location n.
#include <cstdint>
#include <string>
#include <vector>

namespace gpu::spirv {

enum class Stage : uint8_t { Vertex, Pixel, Geometry };

constexpr uint32_t kBindingVsc = 0, kBindingConstants = 1, kBindingTexture0 = 2, kBindingSampler0 = 6;

// false with the compiler's message in err
bool Compile(const std::string& hlsl, const char* entry, Stage stage, std::vector<uint32_t>& spv, std::string& err);

}  // namespace gpu::spirv
