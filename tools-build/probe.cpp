// Compiles generated HLSL through spirv::Compile: does glslang's HLSL front end take what shaders.cpp writes? Each
// shader is also turned into GLSL ES 3.20 (gles_shader.cpp, the OpenGL ES renderer's path) and that is parsed by
// glslang as an ES 3.20 shader; a vertex and a pixel shader are linked together, as the renderer links them.
//   shader_probe          all shaders
//   shader_probe -v       ... and print the first GLSL of each stage
#include <cstdio>
#include <cstring>
#include <string>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "gpu/gles_shader.h"
#include "gpu/shaders.h"
#include "gpu/spirv.h"
using namespace gpu;

static int g_fail = 0, g_ok = 0, g_gl_fail = 0, g_gl_ok = 0;
static bool g_verbose = false;
static std::string g_last_vs, g_last_ps;

static EShLanguage Lang(spirv::Stage st) { return st == spirv::Stage::Vertex ? EShLangVertex : st == spirv::Stage::Pixel ? EShLangFragment : EShLangGeometry; }

// glslang parsing the GLSL as an OpenGL ES 3.20 shader
static bool ParseEs(const std::string& glsl, spirv::Stage st, std::string& err) {
    glslang::TShader sh(Lang(st));
    const char* s[] = {glsl.c_str()};
    sh.setStrings(s, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, Lang(st), glslang::EShClientNone, 0);
    if (!sh.parse(GetDefaultResources(), 320, EEsProfile, false, false, EShMsgDefault)) {
        err = sh.getInfoLog();
        return false;
    }
    return true;
}

static bool LinkEs(const std::string& vs, const std::string& ps, std::string& err) {
    glslang::TShader v(EShLangVertex), f(EShLangFragment);
    const char* a[] = {vs.c_str()};
    const char* b[] = {ps.c_str()};
    v.setStrings(a, 1);
    f.setStrings(b, 1);
    v.setEnvInput(glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientNone, 0);
    f.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientNone, 0);
    if (!v.parse(GetDefaultResources(), 320, EEsProfile, false, false, EShMsgDefault) ||
        !f.parse(GetDefaultResources(), 320, EEsProfile, false, false, EShMsgDefault)) {
        err = std::string(v.getInfoLog()) + f.getInfoLog();
        return false;
    }
    glslang::TProgram p;
    p.addShader(&v);
    p.addShader(&f);
    if (!p.link(EShMsgDefault)) {
        err = p.getInfoLog();
        return false;
    }
    return true;
}

static void Try(const char* name, const std::string& hlsl, spirv::Stage st) {
    std::vector<uint32_t> spv;
    std::string err;
    if (!spirv::Compile(hlsl, "main", st, spv, err)) {
        g_fail++;
        std::printf("FAIL %s\n%.1500s\n", name, err.c_str());
        if (g_fail == 1) std::printf("---- source ----\n%s\n", hlsl.c_str());
        return;
    }
    g_ok++;
    for (int variant = 0; variant < 2; variant++) {
        gles::TranslateOptions o;
        o.clip_control = variant == 1;
        std::string glsl;
        if (!gles::ToGlsl(spv, st, o, glsl, err) || !ParseEs(glsl, st, err)) {
            g_gl_fail++;
            std::printf("GLES FAIL %s (clip control %d)\n%.2000s\n", name, variant, err.c_str());
            if (g_gl_fail == 1) std::printf("---- glsl ----\n%s\n", glsl.c_str());
            continue;
        }
        g_gl_ok++;
        if (variant == 0) (st == spirv::Stage::Vertex ? g_last_vs : g_last_ps) = glsl;
        static bool shown_shadow = false;
        if (g_verbose && !shown_shadow && variant == 0 && glsl.find("sampler2DShadow") != std::string::npos) {
            shown_shadow = true;
            std::printf("---- %s (shadow) ----\n%s\n", name, glsl.c_str());
        }
        static bool shown[3];
        if (g_verbose && !shown[int(st)] && variant == 0) {
            shown[int(st)] = true;
            std::printf("---- %s ----\n%s\n", name, glsl.c_str());
        }
    }
}

static void Link(const char* name) {
    std::string err;
    if (LinkEs(g_last_vs, g_last_ps, err)) {
        g_gl_ok++;
    } else {
        g_gl_fail++;
        std::printf("GLES LINK FAIL %s\n%.2000s\n", name, err.c_str());
    }
}

int main(int argc, char** argv) {
    g_verbose = argc > 1 && !std::strcmp(argv[1], "-v");
    glslang::InitializeProcess();
    // fixed function vertex shaders: untransformed with position+normal+uv, lit and unlit; pretransformed
    FixedKey lit;
    lit.light = 1 | 2;
    lit.types = 1;
    for (uint16_t regs : {uint16_t(0x1), uint16_t(0x5), uint16_t(0x1 | 0x8 | 0x100), uint16_t(0x1 | 0x2 | 0x8 | 0x100 | 0x200)}) {
        Try("fixed-vs", FixedVertexHlsl(regs, false, 0, {}), spirv::Stage::Vertex);
        Try("fixed-vs-lit", FixedVertexHlsl(regs, false, 0, lit), spirv::Stage::Vertex);
        Try("fixed-vs-pretransformed", FixedVertexHlsl(regs, true, 0, {}), spirv::Stage::Vertex);
        Try("fixed-vs-reversed", ReversedDepthHlsl(FixedVertexHlsl(regs, false, 0, lit)), spirv::Stage::Vertex);
        Try("fixed-vs-quad", QuadHlsl(ReversedDepthHlsl(FixedVertexHlsl(regs, true, 0, {}))), spirv::Stage::Vertex);
    }
    Try("fixed-vs-noregs", ReversedDepthHlsl(FixedVertexHlsl(0, true, 0, {})), spirv::Stage::Vertex);
    // a vertex program: mov oPos, v0; mov oD0, v1 is enough to run the microcode translator
    // pixel shaders: fixed function stages with 2D textures of each kind
    for (int kind = 0; kind <= 3; kind++) {
        State s;
        TexInfo tex[4];
        for (auto& t : tex) t.kind = uint8_t(kind);
        s.textures[0] = 0;
        PixelKey k = MakePixelKey(s, tex);
        Try("ps-default", PixelHlsl(k), spirv::Stage::Pixel);
        Try("vs-for-link", ReversedDepthHlsl(FixedVertexHlsl(0x1 | 0x8 | 0x100, false, 0, lit)), spirv::Stage::Vertex);
        Link("fixed-vs + ps-default");
        k.quad = 1;
        Try("ps-quad", PixelHlsl(k), spirv::Stage::Pixel);
        Try("vs-quad-for-link", QuadHlsl(ReversedDepthHlsl(FixedVertexHlsl(0x1 | 0x100, true, 0, {}))), spirv::Stage::Vertex);
        Link("quad vs + ps-quad");
        k.quad = 0;
        k.alphatest = 1;
        Try("ps-alphatest", PixelHlsl(k), spirv::Stage::Pixel);
        k.tex[0].flags |= kTexTarget;
        Try("ps-target", PixelHlsl(k), spirv::Stage::Pixel);
        k.tex[0].shadow = 1;
        Try("ps-shadow", PixelHlsl(k), spirv::Stage::Pixel);
        k.tex[0].shadow = 0;
        k.ff[0][0] = 4;  // some colour op: stage 0 samples its texture
        k.ff[0][1] = 2;  // D3DTA_TEXTURE
        Try("ps-ff-op4", PixelHlsl(k), spirv::Stage::Pixel);
        k.tex[0].flags = 0;
        if (kind == kTex2D) {
            k.tex[0].shadow = 1;
            Try("ps-ff-shadow-sampled", PixelHlsl(k), spirv::Stage::Pixel);
            k.tex[0].shadow = 0;
            k.ff[1][0] = 4;
            k.ff[1][1] = 2;
            k.tex[1].kind = kTexCube;
            Try("ps-ff-2d-cube", PixelHlsl(k), spirv::Stage::Pixel);
        }
    }
    std::printf("compiled %d, failed %d; GLSL ES %d ok, %d failed\n", g_ok, g_fail, g_gl_ok, g_gl_fail);
    return g_fail || g_gl_fail ? 1 : 0;
}
