// SPIR-V to GLSL ES 3.20 for the OpenGL ES backend (gles_shader.h).
#if defined(__ANDROID__) || defined(XBE_GLES_PROBE)

#include "gpu/gles_shader.h"

#include <spirv_glsl.hpp>

#include <exception>

namespace gpu::gles {

bool ToGlsl(const std::vector<uint32_t>& spv, spirv::Stage stage, const TranslateOptions& o, std::string& glsl,
            std::string& err) {
    try {
        spirv_cross::CompilerGLSL c(spv);
        spirv_cross::CompilerGLSL::Options opts = c.get_common_options();
        opts.version = 320;
        opts.es = true;
        opts.vulkan_semantics = false;
        opts.separate_shader_objects = false;
        opts.enable_420pack_extension = false;  // ES 3.1 and later take binding layouts as they are
        opts.vertex.fixup_clipspace = !o.clip_control;
        opts.vertex.flip_vert_y = true;
        opts.vertex.support_nonzero_base_instance = false;
        opts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
        opts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
        c.set_common_options(opts);

        // texture tN with sampler sN becomes one sampler at unit N; a texel fetch without a sampler gets a dummy one
        c.build_dummy_sampler_for_combined_images();
        c.build_combined_image_samplers();
        for (const spirv_cross::CombinedImageSampler& r : c.get_combined_image_samplers()) {
            const uint32_t binding = c.get_decoration(r.image_id, spv::DecorationBinding);
            const uint32_t unit = binding >= spirv::kBindingTexture0 ? binding - spirv::kBindingTexture0 : 0;
            c.set_decoration(r.combined_id, spv::DecorationBinding, unit);
            c.set_name(r.combined_id, "tex_unit" + std::to_string(unit));
        }

        const spirv_cross::ShaderResources res = c.get_shader_resources();
        for (const spirv_cross::Resource& r : res.uniform_buffers) c.unset_decoration(r.id, spv::DecorationDescriptorSet);
        for (const spirv_cross::Resource& r : res.sampled_images) c.unset_decoration(r.id, spv::DecorationDescriptorSet);

        // the screen quad's noperspective input: ES has no such qualifier without the NV extension. Perspective is the
        // same across a quad of one w, which screen quads are
        if (!o.noperspective) {
            auto plain = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list) {
                for (const spirv_cross::Resource& r : list) {
                    c.unset_decoration(r.id, spv::DecorationNoPerspective);
                    const spirv_cross::SPIRType& t = c.get_type(r.base_type_id);
                    for (uint32_t m = 0; m < uint32_t(t.member_types.size()); m++)
                        c.unset_member_decoration(r.base_type_id, m, spv::DecorationNoPerspective);
                }
            };
            plain(stage == spirv::Stage::Pixel ? res.stage_inputs : res.stage_outputs);
        }
        glsl = c.compile();
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool HlslToGlsl(const std::string& hlsl, spirv::Stage stage, const TranslateOptions& o, std::string& glsl,
                std::string& err) {
    std::vector<uint32_t> spv;
    if (!spirv::Compile(hlsl, "main", stage, spv, err)) return false;
    return ToGlsl(spv, stage, o, glsl, err);
}

}  // namespace gpu::gles

#endif
