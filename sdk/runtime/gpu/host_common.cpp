// The parts of the Android renderers that do not depend on the graphics API (host_common.h).
#if defined(__ANDROID__)

#include "gpu/host_common.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "kernel/kernel.h"

namespace gpu::host {

namespace {

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += x[r * 4 + k] * y[k * 4 + c];
            o[r * 4 + c] = v;
        }
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[c * 4 + r] = m[r * 4 + c];
}

// the inverse transpose of the upper 3x3 of a row major 4x4, into the upper 3x3 of n
void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

// D3DLIGHT8 as floats
constexpr int kLightFloats = 26, kLightDiffuse = 1, kLightSpecular = 5, kLightAmbient = 9, kLightPosition = 13,
              kLightDirection = 16, kLightRange = 19, kLightFalloff = 20, kLightAttenuation0 = 21,
              kLightAttenuation1 = 22, kLightAttenuation2 = 23, kLightTheta = 24, kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

WRange ProjectionWRange(const float* m) {
    float m33 = m[10], m34 = m[11], m43 = m[14], m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

void Rgb565(uint16_t c, uint8_t* o) {  // BGRA
    o[2] = uint8_t((c >> 11 & 31) * 255 / 31);
    o[1] = uint8_t((c >> 5 & 63) * 255 / 63);
    o[0] = uint8_t((c & 31) * 255 / 31);
    o[3] = 255;
}

void DecodeColourBlock(const uint8_t* b, uint8_t out[16][4], bool opaque4) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t p[4][4];
    Rgb565(c0, p[0]);
    Rgb565(c1, p[1]);
    if (c0 > c1 || opaque4) {
        for (int i = 0; i < 3; i++) {
            p[2][i] = uint8_t((2 * p[0][i] + p[1][i]) / 3);
            p[3][i] = uint8_t((p[0][i] + 2 * p[1][i]) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) p[2][i] = uint8_t((p[0][i] + p[1][i]) / 2);
        p[2][3] = 255;
        p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
    }
    uint32_t bits = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out[i], p[(bits >> (i * 2)) & 3], 4);
}

constexpr uint64_t kDeclarationSalt = 0x10000, kPretransformedSalt = 0x5A5A, kQuadSalt = 0xC3C30000;
constexpr uint32_t kSigWholeMax = 65536, kSigSamples = 64, kSigSampleBytes = 64;

}  // namespace

// ---- draw constants, as d3d12.cpp fills them ------------------------------------------------------------------------

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? CmpIndex(SimpleState(s, kMAlphaFunc)) : 7.0f;
    c.misc[2] = CmpIndex(s.rs[kRsShadowFunc]);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16], wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w, sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1, oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[kTsProjection].data());
    } else if (SimpleState(s, kMClipMax)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

void FillDrawConstants(const State& s, const DrawKind& k, Topology topology, GuestExtent size, float scale,
                       DrawConstants& c) {
    FillSurfaceConstants(s, size, c);
    FillFogConstants(s, k.prog, c);
    FillCombinerConstants(s, c);
    // ReversedDepthHlsl's zparams: x the w buffer's scale (0 for a z buffer), y its far w, z the polygon offset
    const bool offset = OffsetsPolygons(s, topology);
    const float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
    } else {
        WRange r = DrawWRange(s, k.prog);
        c.zparams[0] = r.near_w / (r.far_w - r.near_w);
        c.zparams[1] = r.far_w;
        c.zparams[2] = k.prog ? units : units * r.far_w / s.zscale;
    }
    // render target reads move back the half pixel the XDK's screen space offset adds
    const bool shifted = !k.prog || ReadsViewportOffset(s.vs->program);
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) {
            c.texsize[i][0] = float(d.width);
            c.texsize[i][1] = float(d.height);
        }
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
        c.rtshift[i] = shifted ? -0.5f * scale : 0.0f;  // host pixels
    }
    if (k.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, k.fk, c);
    }
}

// ---- shader keys ----------------------------------------------------------------------------------------------------

DrawKind ClassifyDraw(const State& s, const Assembled& a, uint32_t count) {
    DrawKind k;
    k.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    const bool points = a.topology == Topology::Points;
    k.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    k.fixed_transform = !k.prog && !a.pretransformed;
    if (!k.prog) k.fk = MakeFixedKey(s);
    if (k.prog && !s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
    return k;
}

uint64_t VertexKey(const State& s, const Assembled& a, const DrawKind& k) {
    uint64_t seed = k.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!k.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (k.quad) vkey ^= kQuadSalt;
    if (!k.prog) vkey = Hash(&k.fk, sizeof(k.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawKind& k) {
    std::string src = ReversedDepthHlsl(k.prog ? VertexProgramHlsl(s.vs->program, a.regs, a.decode)
                                               : FixedVertexHlsl(a.regs, a.pretransformed, a.decode, k.fk));
    return k.quad ? QuadHlsl(std::move(src)) : src;
}

// ---- textures -------------------------------------------------------------------------------------------------------

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B, kPaletteCommon = 0, kPaletteData = 4, kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

void DecodeBc(Host kind, const std::vector<uint8_t>& in, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint32_t block = kind == Host::BC1 ? 8 : 16;
    out.assign(size_t(bw) * 4 * bh * 4 * 4, 0);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            size_t at = (size_t(by) * bw + bx) * block;
            if (at + block > in.size()) return;
            const uint8_t* b = &in[at];
            uint8_t px[16][4];
            DecodeColourBlock(kind == Host::BC1 ? b : b + 8, px, kind != Host::BC1);
            if (kind == Host::BC2) {
                for (int i = 0; i < 16; i++) px[i][3] = uint8_t((b[i / 2] >> ((i & 1) * 4) & 15) * 17);
            } else if (kind == Host::BC3) {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) {
                    for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                } else {
                    for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
                    a[6] = 0;
                    a[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (i * 3)) & 7];
            }
            for (int y = 0; y < 4; y++)
                std::memcpy(&out[((size_t(by) * 4 + y) * bw * 4 + size_t(bx) * 4) * 4], px[y * 4], 16);
        }
}

// ---- perf -----------------------------------------------------------------------------------------------------------

void LogPerf(Perf& p, PerfClock::time_point& start) {
    const double el = std::chrono::duration<double>(PerfClock::now() - start).count();
    if (el < 2.0) return;
    const double n = double(std::max<uint64_t>(p.frames, 1));
    LOGI("perf: %.1f fps | per frame: draw %.2f ms (bind %.2f, tex %.2f) shader %.2f pso %.2f | wait %.2f present %.2f | "
         "%.0f draws, %llu shaders %llu psos %llu texture uploads %llu flushes in %.1f s",
         double(p.frames) / el, p.draw_ms / n, p.bind_ms / n, p.tex_ms / n, p.compile_ms / n, p.pso_ms / n, p.wait_ms / n,
         p.present_ms / n, double(p.draws) / n, (unsigned long long)p.shaders, (unsigned long long)p.psos,
         (unsigned long long)p.texups, (unsigned long long)p.flushes, el);
    p = Perf{};
    start = PerfClock::now();
}

}  // namespace gpu::host

#endif  // __ANDROID__
