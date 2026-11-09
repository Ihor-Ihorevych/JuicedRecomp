#pragma once
// What the Android renderers (vulkan.cpp, gles.cpp) share: the XDK state turned into DrawConstants, the shader keys of a
// draw, texture signatures and palettes, DXT decoding for GPUs without BC formats, and frame pacing.
#if defined(__ANDROID__)

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"

namespace gpu::host {

constexpr uint32_t kQuadVertices = 6;
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;
constexpr uint32_t kZUseW = 2;
constexpr uint32_t kTexfLinear = 2, kTexfAnisotropic = 3;
constexpr uint32_t kBlendDestColor = 0x306;
constexpr uint32_t kCullCw = 2;
constexpr uint32_t kFillWireframe = 2;
constexpr uint32_t kMClipMin = 0x394, kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;
constexpr size_t kTsView = 2, kTsProjection = 3, kTsTexture0 = 16, kTsWorld = 256;

inline float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// console ARGB to RGBA floats
inline void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

inline uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }
// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 as 0 to 7, ALWAYS for anything else
inline uint32_t CmpIndexU(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
inline float CmpIndex(uint32_t v) { return float(CmpIndexU(v)); }
// the same compare with depth stored reversed, as a D3DCMP value
inline uint32_t ReversedCmpValue(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return 0x200 + mirror[CmpIndexU(v)];
}

inline TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr uint32_t kFmtD16 = 0x2C, kFmtLinD16 = 0x30;
inline uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

struct GuestExtent {
    uint32_t w, h;
};

struct WRange {
    float near_w, far_w;
};
WRange DrawWRange(const State& s, bool program);
bool OffsetsPolygons(const State& s, Topology topology);

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c);
void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillCombinerConstants(const State& s, DrawConstants& c);
void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c);

// the shaders a draw needs, before their HLSL is written
struct DrawKind {
    bool prog = false, quad = false, fixed_transform = false;
    FixedKey fk;
};
DrawKind ClassifyDraw(const State& s, const Assembled& a, uint32_t count);
uint64_t VertexKey(const State& s, const Assembled& a, const DrawKind& k);
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;
// the vertex shader's HLSL for the draw, depth reversed (ReversedDepthHlsl), a screen quad's when k.quad
std::string VertexHlsl(const State& s, const Assembled& a, const DrawKind& k);

// every DrawConstants field a draw sets; scale is the host pixels per console pixel (across + down) / 2
void FillDrawConstants(const State& s, const DrawKind& k, Topology topology, GuestExtent size, float scale,
                       DrawConstants& c);

// the bytes of a texture, hashed whole up to 64 KiB and sampled past that
uint64_t MemSig(uint32_t data, uint32_t total);

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette);

inline uint32_t TexelBytes(Host h) { return h == Host::R16Unorm ? 2 : 4; }
inline uint32_t BlockBytes(Host h) { return h == Host::BC1 ? 8 : 16; }
inline bool IsBc(Host h) { return h == Host::BC1 || h == Host::BC2 || h == Host::BC3; }

// BC1, BC2, BC3 blocks of w x h texels (whole blocks) to BGRA rows
void DecodeBc(Host kind, const std::vector<uint8_t>& in, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

// where the picture goes on a screen of this size: the DisplayAspect rectangle, centred
struct PictureRect {
    int32_t x, y, w, h;
};
PictureRect PictureOnScreen(uint32_t screen_w, uint32_t screen_h);
// host pixels per console pixel for the resolution setting, across and down
void RenderScale(uint32_t screen_w, uint32_t screen_h, uint32_t guest_w, uint32_t guest_h, float& sx, float& sy);

using PerfClock = std::chrono::steady_clock;

// where a second of frames goes, logged by Present
struct Perf {
    double draw_ms = 0, compile_ms = 0, pso_ms = 0, wait_ms = 0, present_ms = 0, tex_ms = 0, bind_ms = 0;
    uint64_t frames = 0, draws = 0, shaders = 0, psos = 0, texups = 0, texup_bytes = 0, flushes = 0, rendering = 0, sets = 0;
};
struct PerfTimer {
    double& acc;
    PerfClock::time_point t = PerfClock::now();
    explicit PerfTimer(double& a) : acc(a) {}
    ~PerfTimer() { acc += std::chrono::duration<double, std::milli>(PerfClock::now() - t).count(); }
};
// a second of frames: where the time went, in ms per frame; resets p and start once logged
void LogPerf(Perf& p, PerfClock::time_point& start);

// After a present: measures the frame rate (GetFps), and waits out the rest of the frame when a limit is set
class FramePacer {
public:
    void Pace();

private:
    PerfClock::time_point next_{}, fps_start_ = PerfClock::now();
    uint64_t frames_ = 0;
};

}  // namespace gpu::host

#endif  // __ANDROID__
