#include "gpu/spirv.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <mutex>
#include <regex>

namespace gpu::spirv {

namespace {

std::once_flag g_init;

EShLanguage Language(Stage s) {
    switch (s) {
        case Stage::Vertex: return EShLangVertex;
        case Stage::Pixel: return EShLangFragment;
        default: return EShLangGeometry;
    }
}

// "float4 v3 : ATTR3;" becomes "[[vk::location(3)]] float4 v3 : ATTR3;": glslang numbers HLSL inputs by declaration
// order otherwise, and the input layout is built from the register numbers
std::string PinLocations(const std::string& src) {
    static const std::regex attr(R"((\b(?:float4|int4|uint)\s+\w+\s*:\s*ATTR(\d+)\s*;))");
    return std::regex_replace(src, attr, "[[vk::location($2)]] $1");
}

}  // namespace

bool Compile(const std::string& hlsl, const char* entry, Stage stage, std::vector<uint32_t>& spv, std::string& err) {
    std::call_once(g_init, [] { glslang::InitializeProcess(); });
    const EShLanguage lang = Language(stage);
    const std::string text = stage == Stage::Vertex ? PinLocations(hlsl) : hlsl;
    const char* strings[] = {text.c_str()};

    glslang::TShader shader(lang);
    shader.setStrings(strings, 1);
    shader.setEntryPoint(entry);
    shader.setSourceEntryPoint(entry);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    // b0, b1 at bindings 0 and 1; t0-t3 from 2; s0-s3 from 6: every register type is shifted on its own
    shader.setShiftBinding(glslang::EResUbo, 0);
    shader.setShiftBinding(glslang::EResTexture, kBindingTexture0);
    shader.setShiftBinding(glslang::EResSampler, kBindingSampler0);

    const EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        err = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages) || !program.mapIO()) {
        err = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = false;
    options.optimizeSize = false;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spv, &options);
    return !spv.empty();
}

}  // namespace gpu::spirv
