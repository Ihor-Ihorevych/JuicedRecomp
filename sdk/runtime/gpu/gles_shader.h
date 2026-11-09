#pragma once
// The renderer's HLSL (shaders.cpp) for OpenGL ES 3.2: compiled to SPIR-V as for Vulkan (spirv.cpp), then written out
// as GLSL ES 3.20 by SPIRV-Cross.
//
// What the GLSL looks like to the backend (gles.cpp):
//   uniform block VC  binding 0   the vertex constants (b0)
//   uniform block DC  binding 1   DrawConstants (b1)
//   sampler units 0-3              t0-t3 each with its s0-s3, combined
//   vertex inputs                  ATTRn at location n
//   clip space                     y flipped (gl_Position.y = -y), so every render target holds its rows top first as
//                                  on the console and Vulkan; z from D3D's 0..w to GL's -w..w unless clip_control
//                                  (GL_EXT_clip_control's zero-to-one depth) is on
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/spirv.h"

namespace gpu::gles {

struct TranslateOptions {
    bool clip_control = false;   // GL_EXT_clip_control with GL_ZERO_TO_ONE: z is left as D3D writes it
    bool noperspective = false;  // GL_NV_shader_noperspective_interpolation: kept, else such inputs are perspective
};

// spv: spirv::Compile's output for the stage; false with the reason in err
bool ToGlsl(const std::vector<uint32_t>& spv, spirv::Stage stage, const TranslateOptions& o, std::string& glsl,
            std::string& err);

// HLSL straight to GLSL ES
bool HlslToGlsl(const std::string& hlsl, spirv::Stage stage, const TranslateOptions& o, std::string& glsl,
                std::string& err);

}  // namespace gpu::gles
