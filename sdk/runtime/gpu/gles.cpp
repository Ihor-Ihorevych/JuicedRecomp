// The title's Direct3D 8 calls drawn with OpenGL ES 3.2, for Android phones whose Vulkan driver breaks.
//
// It follows vulkan.cpp draw for draw: the same state translation (host_common.cpp), the same HLSL (shaders.cpp) taken
// through SPIR-V to GLSL ES (gles_shader.cpp). The conventions that differ from Vulkan:
//   - every shader flips clip y, so a render target holds its rows top first like the console's and Vulkan's: viewport,
//     scissor and copy rectangles are the console's rows as they are, texture coordinates need nothing, and only Present
//     flips (the window's rows start at the bottom). Triangle winding is seen mirrored, so the front face is swapped.
//   - depth is stored reversed as on Vulkan. With GL_EXT_clip_control the shaders write 0..1 depth as they do there;
//     without it they map it to GL's -1..1 and the depth range maps it back.
//   - guest textures in BGRA are uploaded as RGBA with the red and blue channels swizzled back when sampled.
// What is left out is listed in docs/android-vulkan.md, under OpenGL ES.
#if defined(__ANDROID__)

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "dxgiformat.h"
#include "gpu/android_platform.h"
#include "gpu/backend.h"
#include "gpu/gles_shader.h"
#include "gpu/host_common.h"
#include "gpu/shaders.h"
#include "gpu/spirv.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

namespace gpu {

namespace {

using namespace host;

constexpr int kFrames = 3;                       // frames the CPU may write ahead of the GPU
constexpr size_t kVertexRegion = 24u << 20;      // per frame: vertices
constexpr size_t kIndexRegion = 4u << 20;        // per frame: indices
constexpr size_t kUniformRegion = 16u << 20;     // per frame: constants
constexpr float kDepthFar = 0;                   // reversed depth
constexpr GLenum kDepthFormat = GL_DEPTH32F_STENCIL8;

// GL_EXT_clip_control, GL_EXT_buffer_storage, GL_EXT_texture_norm16 and the S3TC formats by value: not every NDK header
// names them
constexpr GLenum kLowerLeft = 0x8CA1, kZeroToOne = 0x935F;
constexpr GLbitfield kMapPersistent = 0x0040, kMapCoherent = 0x0080;
constexpr GLenum kR16 = 0x822A;
constexpr GLenum kDxt1 = 0x83F1, kDxt3 = 0x83F2, kDxt5 = 0x83F3;
constexpr GLenum kMaxAnisotropy = 0x84FE, kMaxAnisotropyLimit = 0x84FF;
using ClipControlFn = void(GL_APIENTRYP)(GLenum origin, GLenum depth);
using BufferStorageFn = void(GL_APIENTRYP)(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);

// ---------------------------------------------------------------------------------------------------------------------
// state translation: the console's values are mostly OpenGL's own (the NV2A takes them as they are), checked here

GLenum Blend(uint32_t v, bool dst) {
    switch (v) {
        case 0: return GL_ZERO;
        case 1: return GL_ONE;
        case 0x300: return GL_SRC_COLOR;
        case 0x301: return GL_ONE_MINUS_SRC_COLOR;
        case 0x302: return GL_SRC_ALPHA;
        case 0x303: return GL_ONE_MINUS_SRC_ALPHA;
        case 0x304: return GL_DST_ALPHA;
        case 0x305: return GL_ONE_MINUS_DST_ALPHA;
        case kBlendDestColor: return GL_DST_COLOR;
        case 0x307: return GL_ONE_MINUS_DST_COLOR;
        case 0x308: return dst ? GL_ONE : GL_SRC_ALPHA_SATURATE;  // a source factor only in ES
        case 0x8001: return GL_CONSTANT_COLOR;
        case 0x8002: return GL_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return GL_CONSTANT_ALPHA;
        case 0x8004: return GL_ONE_MINUS_CONSTANT_ALPHA;
        default: return GL_ONE;
    }
}

GLenum BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return GL_FUNC_SUBTRACT;
        case 0x800B: return GL_FUNC_REVERSE_SUBTRACT;
        case 0x8007: return GL_MIN;
        case 0x8008: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 are GL_NEVER to GL_ALWAYS
GLenum Cmp(uint32_t v) { return GL_NEVER + CmpIndexU(v); }
GLenum ReversedCmp(uint32_t v) { return Cmp(ReversedCmpValue(v)); }

GLenum StencilOp(uint32_t v) {
    switch (v) {
        case 0: return GL_ZERO;
        case 0x1E01: return GL_REPLACE;
        case 0x1E02: return GL_INCR;
        case 0x1E03: return GL_DECR;
        case 0x150A: return GL_INVERT;
        case 0x8507: return GL_INCR_WRAP;
        case 0x8508: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

GLenum Address(uint32_t v) {
    switch (v) {
        case 2: return GL_MIRRORED_REPEAT;
        case 3: case 5: return GL_CLAMP_TO_EDGE;
        case 4: return GL_CLAMP_TO_BORDER;
        default: return GL_REPEAT;
    }
}

// a vertex element as glVertexAttrib(I)Pointer takes it
struct AttribFormat {
    GLint size;
    GLenum type;
    bool normalized, integer, bgra;
};

AttribFormat VertexFormat(uint8_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32_FLOAT: return {1, GL_FLOAT, false, false, false};
        case DXGI_FORMAT_R32G32_FLOAT: return {2, GL_FLOAT, false, false, false};
        case DXGI_FORMAT_R32G32B32_FLOAT: return {3, GL_FLOAT, false, false, false};
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return {4, GL_FLOAT, false, false, false};
        case DXGI_FORMAT_B8G8R8A8_UNORM: return {4, GL_UNSIGNED_BYTE, true, false, true};  // bytes swapped on upload
        case DXGI_FORMAT_R16_SNORM: return {1, GL_SHORT, true, false, false};
        case DXGI_FORMAT_R16G16_SNORM: return {2, GL_SHORT, true, false, false};
        case DXGI_FORMAT_R16G16B16A16_SNORM: return {4, GL_SHORT, true, false, false};
        case DXGI_FORMAT_R8_UNORM: return {1, GL_UNSIGNED_BYTE, true, false, false};
        case DXGI_FORMAT_R8G8_UNORM: return {2, GL_UNSIGNED_BYTE, true, false, false};
        case DXGI_FORMAT_R8G8B8A8_UNORM: return {4, GL_UNSIGNED_BYTE, true, false, false};
        case DXGI_FORMAT_R16_SINT: return {1, GL_SHORT, false, true, false};
        case DXGI_FORMAT_R16G16_SINT: return {2, GL_SHORT, false, true, false};
        case DXGI_FORMAT_R16G16B16A16_SINT: return {4, GL_SHORT, false, true, false};
        case DXGI_FORMAT_R32_UINT: return {1, GL_UNSIGNED_INT, false, true, false};
        default: return {4, GL_FLOAT, false, false, false};
    }
}

GLenum TexTarget(TexKind k) { return k == kTex3D ? GL_TEXTURE_3D : k == kTexCube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D; }

const char* GlError(GLenum e) {
    switch (e) {
        case GL_INVALID_ENUM: return "invalid enum";
        case GL_INVALID_VALUE: return "invalid value";
        case GL_INVALID_OPERATION: return "invalid operation";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "invalid framebuffer operation";
        case GL_OUT_OF_MEMORY: return "out of memory";
        default: return "?";
    }
}

// errors are logged with where they were seen, the first few of each place only
#define GL_CHECK(where)                                                       \
    do {                                                                      \
        for (GLenum e_; (e_ = glGetError()) != GL_NO_ERROR;)                  \
            LOG_FIRST(8, "gles: %s at %s (0x%04X)", GlError(e_), where, e_);  \
    } while (0)

// ---------------------------------------------------------------------------------------------------------------------
// the backend

struct GlTarget {
    GLuint tex = 0;
    bool depth = false;
    uint32_t w = 0, h = 0;    // the host texture: the guest size times the render scale
    uint32_t gw = 0, gh = 0;  // the console surface's size, which the title's rects and viewports are in
    uint32_t data = 0;
};

struct GlTex {
    GLuint tex = 0;
    TextureDesc d;
    GLenum target = GL_TEXTURE_2D, internal = GL_RGBA8;
    uint32_t levels = 1, pad_w = 0, pad_h = 0;
    uint64_t sig = 0, frame = ~0ull, palette = 0;
    bool decode_bc = false, compressed = false, r16_as_float = false, unfilterable = false;
};

struct DrawShaders : DrawKind {
    uint64_t vkey = 0, vh = 0, pkey = 0, ph = 0;
    GLuint vs = 0, ps = 0;
};

struct DrawTextures {
    TexInfo info[4];
    GLenum target[4] = {GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D, GL_TEXTURE_2D};
    GLuint tex[4] = {};
    bool unfilterable[4] = {};
};

// a per frame region of a streaming buffer: written by the CPU, read by draws of that frame
class Ring {
public:
    void Init(GLenum target, size_t region, GLuint align, BufferStorageFn storage) {
        target_ = target;
        region_ = region;
        align_ = std::max<GLuint>(align, 4);
        glGenBuffers(1, &buf_);
        glBindBuffer(target_, buf_);
        const size_t total = region_ * kFrames;
        if (storage) {
            storage(target_, GLsizeiptr(total), nullptr, GL_MAP_WRITE_BIT | kMapPersistent | kMapCoherent);
            map_ = static_cast<uint8_t*>(
                glMapBufferRange(target_, 0, GLsizeiptr(total), GL_MAP_WRITE_BIT | kMapPersistent | kMapCoherent));
        }
        if (!map_) glBufferData(target_, GLsizeiptr(total), nullptr, GL_STREAM_DRAW);
    }
    // the frame's region starts over
    void Begin(int frame) {
        base_ = size_t(frame % kFrames) * region_;
        used_ = 0;
    }
    bool Fits(size_t n) const { return ((used_ + align_ - 1) & ~size_t(align_ - 1)) + n <= region_; }
    // data copied in, its offset in the buffer; Fits must have been checked
    size_t Put(const void* data, size_t n) {
        const size_t at = base_ + ((used_ + align_ - 1) & ~size_t(align_ - 1));
        used_ = at - base_ + n;
        if (map_) {
            std::memcpy(map_ + at, data, n);
        } else {
            glBindBuffer(target_, buf_);
            void* p = glMapBufferRange(target_, GLintptr(at), GLsizeiptr(n),
                                       GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
            if (p) {
                std::memcpy(p, data, n);
                glUnmapBuffer(target_);
            }
        }
        return at;
    }
    void Reset() { used_ = 0; }  // after the GPU finished everything in the region
    GLuint buffer() const { return buf_; }
    bool persistent() const { return map_ != nullptr; }

private:
    GLenum target_ = GL_ARRAY_BUFFER;
    GLuint buf_ = 0;
    GLuint align_ = 4;
    uint8_t* map_ = nullptr;
    size_t region_ = 0, base_ = 0, used_ = 0;
};

class GlesBackend final : public Backend {
public:
    ~GlesBackend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t, uint32_t) override { LOG_FIRST(1, "gles: read-back of a render target is not implemented"); }
    void SyncLate(uint32_t) override {}
    void Flush() override;
    void SetGamma(const uint8_t*) override {}
    void GetReport(uint32_t report) override;
    void WaitGpuFrame(uint64_t n) override;

private:
    // setup
    bool CreateContext();
    bool CreateWindowSurface(ANativeWindow* window);
    bool MakeCurrent();
    void LoseSurface();
    bool CreateDummies();
    bool HasExtension(const char* name) const { return extensions_.find(std::string(" ") + name + " ") != std::string::npos; }
    // frame
    void EndFrame();      // a fence for the frame just drawn
    void NextFrame();     // the next frame's ring regions, once the GPU is done with them
    void WaitAll();       // the GPU finished everything recorded so far
    void ApplyScale();
    // targets
    GlTarget* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(GlTarget& t);
    bool BindFramebuffer(GlTarget* rt, GlTarget* ds);
    // draw
    GLuint Shader(const std::string& hlsl, spirv::Stage stage, uint64_t& hash);
    GLuint Program(uint64_t vh, uint64_t ph, GLuint vs, GLuint ps);
    bool BindTexture(const State& s, int stage, DrawTextures& stages);
    void UploadTexture(GlTex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* guest, const uint32_t* palette);
    GLuint Sampler(const State& s, int stage, const DrawTextures& stages);
    void ApplyState(const State& s, const Assembled& a, bool has_rt, bool has_ds);
    void SetScissor(bool on);

    // EGL
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLSurface pbuffer_ = EGL_NO_SURFACE;  // current while there is no window, when surfaceless contexts are missing
    ANativeWindow* window_ = nullptr;
    bool surfaceless_ = false;
    PFNEGLCREATESYNCKHRPROC create_sync_ = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync_ = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC wait_sync_ = nullptr;
    EGLint surf_w_ = 0, surf_h_ = 0;

    // capabilities
    std::string extensions_;
    bool clip_control_ = false, s3tc_ = false, norm16_ = false, float_linear_ = false, anisotropic_ = false,
         noperspective_ = false;
    float max_anisotropy_ = 1;
    GLint ubo_align_ = 256;

    // frames: a fence per frame slot, any thread may wait on one (WaitGpuFrame)
    std::mutex fence_lock_;
    std::array<EGLSyncKHR, kFrames> fences_{};
    std::array<uint64_t, kFrames> fence_frame_{};
    uint64_t presents_ = 0;
    Ring vertices_, indices_, uniforms_;
    std::vector<uint8_t> staging_;
    std::array<uint32_t, 192 * 4> last_vsc_{};
    size_t last_vsc_at_ = ~size_t(0);
    DrawConstants last_dc_{};
    size_t last_dc_at_ = ~size_t(0);

    // resources
    std::unordered_map<uint64_t, GlTarget> targets_;
    std::unordered_map<uint64_t, GlTex> textures_;
    std::unordered_map<uint64_t, GLuint> shaders_;  // by the hash of the HLSL
    struct ShaderEntry {
        GLuint shader;
        uint64_t hash;
    };
    std::unordered_map<uint64_t, ShaderEntry> shader_keys_;  // by the draw's shader key: no HLSL written on a hit
    std::unordered_map<uint64_t, GLuint> programs_;
    std::unordered_map<uint64_t, GLuint> samplers_;
    std::unordered_map<uint64_t, GLuint> framebuffers_;  // by colour texture << 32 | depth texture
    GLuint dummy_[4] = {};                               // by TexKind
    GLuint vao_ = 0;
    GLuint present_fbo_ = 0;
    GlTarget* bound_rt_ = nullptr;
    GlTarget* bound_ds_ = nullptr;
    GLuint bound_fbo_ = ~0u;
    uint16_t enabled_attribs_ = 0;

    Assembled asm_;
    Perf perf_;
    PerfClock::time_point perf_start_ = PerfClock::now();
    FramePacer pacer_;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float sx_ = 1, sy_ = 1;  // host pixels per console pixel, across and down
};

GlesBackend::~GlesBackend() {
    if (dpy_ == EGL_NO_DISPLAY) return;
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    // the rest goes with the process
}

// ---- setup ----------------------------------------------------------------------------------------------------------

bool GlesBackend::CreateContext() {
    dpy_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy_ == EGL_NO_DISPLAY || !eglInitialize(dpy_, nullptr, nullptr)) {
        LOGE("gles: no EGL display");
        return false;
    }
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                            EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(dpy_, attrs, &config_, 1, &n) || n < 1) {
        LOGE("gles: no RGBA8 OpenGL ES 3 config");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
    ctx_ = eglCreateContext(dpy_, config_, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx_ == EGL_NO_CONTEXT) {
        LOGE("gles: no OpenGL ES 3.2 context (EGL error 0x%X)", eglGetError());
        return false;
    }
    const char* egl_ext = eglQueryString(dpy_, EGL_EXTENSIONS);
    const std::string ee = std::string(" ") + (egl_ext ? egl_ext : "") + " ";
    surfaceless_ = ee.find(" EGL_KHR_surfaceless_context ") != std::string::npos;
    if (ee.find(" EGL_KHR_fence_sync ") != std::string::npos) {
        create_sync_ = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        destroy_sync_ = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        wait_sync_ = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    }
    if (!create_sync_ || !destroy_sync_ || !wait_sync_) {
        LOGE("gles: EGL_KHR_fence_sync is missing");
        return false;
    }
    if (!surfaceless_) {
        const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        pbuffer_ = eglCreatePbufferSurface(dpy_, config_, pb);
    }
    return true;
}

bool GlesBackend::CreateWindowSurface(ANativeWindow* window) {
    if (!window) return false;
    surface_ = eglCreateWindowSurface(dpy_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("gles: eglCreateWindowSurface failed (0x%X)", eglGetError());
        return false;
    }
    window_ = window;
    eglQuerySurface(dpy_, surface_, EGL_WIDTH, &surf_w_);
    eglQuerySurface(dpy_, surface_, EGL_HEIGHT, &surf_h_);
    return true;
}

// the context on this thread (Init runs on the title's thread, the rest on the render thread), drawing to the window
// when there is one
bool GlesBackend::MakeCurrent() {
    if (eglGetCurrentContext() == ctx_ && eglGetCurrentSurface(EGL_DRAW) == (surface_ ? surface_ : pbuffer_)) return true;
    EGLSurface s = surface_ != EGL_NO_SURFACE ? surface_ : pbuffer_;
    if (!eglMakeCurrent(dpy_, s, s, ctx_)) {
        LOGE("gles: eglMakeCurrent failed (0x%X)", eglGetError());
        return false;
    }
    if (surface_) eglSwapInterval(dpy_, 1);
    return true;
}

// the window went away with the app: drawing goes on into the targets, without a picture, until a new window comes
void GlesBackend::LoseSurface() {
    if (surface_ == EGL_NO_SURFACE) return;
    LOGI("gles: surface lost (window %p)", (void*)window_);
    EGLSurface none = pbuffer_;
    eglMakeCurrent(dpy_, none, none, ctx_);
    eglDestroySurface(dpy_, surface_);
    surface_ = EGL_NO_SURFACE;
    window_ = nullptr;
}

bool GlesBackend::CreateDummies() {
    const uint32_t zero[6] = {};
    for (int k = kTex2D; k <= kTexCube; k++) {
        const GLenum target = TexTarget(TexKind(k));
        glGenTextures(1, &dummy_[k]);
        glBindTexture(target, dummy_[k]);
        if (k == kTex3D) {
            glTexStorage3D(target, 1, GL_RGBA8, 1, 1, 1);
            glTexSubImage3D(target, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        } else {
            glTexStorage2D(target, 1, GL_RGBA8, 1, 1);
            if (k == kTexCube) {
                for (int f = 0; f < 6; f++)
                    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
            } else {
                glTexSubImage2D(target, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
            }
        }
    }
    dummy_[kTexNone] = dummy_[kTex2D];
    GL_CHECK("dummy textures");
    return true;
}

bool GlesBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    ANativeWindow* window = GetAndroidWindow();
    if (!window) {
        LOGE("gles: no Android window; gpu::SetAndroidWindow must run before the title starts");
        return false;
    }
    if (!CreateContext() || !CreateWindowSurface(window) || !MakeCurrent()) return false;

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    LOGI("gles: %s, %s, %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION),
         (const char*)glGetString(GL_VENDOR));
    if (major * 10 + minor < 32) {
        LOGE("gles: OpenGL ES %d.%d, the renderer needs 3.2", major, minor);
        return false;
    }
    GLint next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    extensions_ = " ";
    for (GLint i = 0; i < next; i++) {
        extensions_ += reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        extensions_ += ' ';
    }
    auto clip = reinterpret_cast<ClipControlFn>(eglGetProcAddress("glClipControlEXT"));
    clip_control_ = HasExtension("GL_EXT_clip_control") && clip;
    if (clip_control_) clip(kLowerLeft, kZeroToOne);
    s3tc_ = HasExtension("GL_EXT_texture_compression_s3tc") ||
            (HasExtension("GL_EXT_texture_compression_dxt1") && HasExtension("GL_ANGLE_texture_compression_dxt3") &&
             HasExtension("GL_ANGLE_texture_compression_dxt5"));
    norm16_ = HasExtension("GL_EXT_texture_norm16");
    float_linear_ = HasExtension("GL_OES_texture_float_linear");
    anisotropic_ = HasExtension("GL_EXT_texture_filter_anisotropic");
    noperspective_ = HasExtension("GL_NV_shader_noperspective_interpolation");
    if (anisotropic_) glGetFloatv(kMaxAnisotropyLimit, &max_anisotropy_);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align_);
    auto storage = HasExtension("GL_EXT_buffer_storage")
                       ? reinterpret_cast<BufferStorageFn>(eglGetProcAddress("glBufferStorageEXT"))
                       : nullptr;
    LOGI("gles: clip control %s, BC textures %s, norm16 %s, float linear %s, anisotropy %.0f, buffer storage %s",
         clip_control_ ? "yes" : "no", s3tc_ ? "native" : "decoded on the CPU", norm16_ ? "yes" : "no",
         float_linear_ ? "yes" : "no", anisotropic_ ? max_anisotropy_ : 0.0f, storage ? "yes" : "no");

    vertices_.Init(GL_ARRAY_BUFFER, kVertexRegion, 16, storage);
    indices_.Init(GL_ELEMENT_ARRAY_BUFFER, kIndexRegion, 4, storage);
    uniforms_.Init(GL_UNIFORM_BUFFER, kUniformRegion, GLuint(ubo_align_), storage);
    vertices_.Begin(0);
    indices_.Begin(0);
    uniforms_.Begin(0);
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenFramebuffers(1, &present_fbo_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glDisable(GL_DITHER);
    if (!CreateDummies()) return false;
    ApplyScale();
    GL_CHECK("init");
    LOGI("gles: ready, surface %dx%d", surf_w_, surf_h_);
    // the render thread takes the context over
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return true;
}

// ---- frames ---------------------------------------------------------------------------------------------------------

void GlesBackend::EndFrame() {
    EGLSyncKHR sync = create_sync_(dpy_, EGL_SYNC_FENCE_KHR, nullptr);
    glFlush();  // the fence must reach the GPU for another thread's wait to end
    std::lock_guard<std::mutex> lock(fence_lock_);
    const int slot = int(presents_ % kFrames);
    if (fences_[slot]) destroy_sync_(dpy_, fences_[slot]);  // a frame kFrames back, waited for by NextFrame already
    fences_[slot] = sync;
    fence_frame_[slot] = presents_;
}

void GlesBackend::NextFrame() {
    const int slot = int(presents_ % kFrames);  // presents_ already counts the frame just ended
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    {
        std::lock_guard<std::mutex> lock(fence_lock_);
        sync = fences_[slot];
    }
    if (sync) {
        PerfTimer t(perf_.wait_ms);
        wait_sync_(dpy_, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
    }
    vertices_.Begin(slot);
    indices_.Begin(slot);
    uniforms_.Begin(slot);
    last_vsc_at_ = last_dc_at_ = ~size_t(0);
}

// a frame whose rings ran out: everything so far is waited for, and the frame's regions start over
void GlesBackend::WaitAll() {
    perf_.flushes++;
    EGLSyncKHR sync = create_sync_(dpy_, EGL_SYNC_FENCE_KHR, nullptr);
    wait_sync_(dpy_, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
    destroy_sync_(dpy_, sync);
    vertices_.Reset();
    indices_.Reset();
    uniforms_.Reset();
    last_vsc_at_ = last_dc_at_ = ~size_t(0);
}

void GlesBackend::Flush() {
    if (!MakeCurrent()) return;
    glFlush();
}

void GlesBackend::WaitGpuFrame(uint64_t n) {
    // any thread: EGL fences may be waited on without the context
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    {
        std::lock_guard<std::mutex> lock(fence_lock_);
        const int slot = int(n % kFrames);
        if (fences_[slot] && fence_frame_[slot] == n) sync = fences_[slot];
    }
    if (sync) wait_sync_(dpy_, sync, 0, EGL_FOREVER_KHR);
}

void GlesBackend::GetReport(uint32_t report) {
    constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
    kernel::W32(report + kTime, uint32_t(presents_));
    kernel::W32(report + kTimeHigh, 0);
    kernel::W32(report + kCount, 0x100);
    kernel::W32(report + kStatus, 0);
}

// ---- targets --------------------------------------------------------------------------------------------------------

void GlesBackend::ReleaseTarget(GlTarget& t) {
    if (bound_rt_ == &t) bound_rt_ = nullptr;
    if (bound_ds_ == &t) bound_ds_ = nullptr;
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (uint32_t(it->first >> 32) == t.tex || uint32_t(it->first) == t.tex) {
            if (bound_fbo_ == it->second) bound_fbo_ = ~0u;
            glDeleteFramebuffers(1, &it->second);
            it = framebuffers_.erase(it);
        } else {
            ++it;
        }
    }
    glDeleteTextures(1, &t.tex);
    t.tex = 0;
}

GlTarget* GlesBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    if (!gw || !gh) return nullptr;
    const uint64_t key = uint64_t(s.data) << 1 | (depth ? 1 : 0);
    auto it = targets_.find(key);
    if (it != targets_.end()) {
        if (it->second.gw == gw && it->second.gh == gh) return &it->second;
        ReleaseTarget(it->second);
        targets_.erase(it);
    }
    GlTarget t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * sx_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * sy_)));
    t.data = s.data;
    LOGI("gles: new %s target %08X %ux%u at %ux%u", depth ? "depth" : "colour", s.data, gw, gh, t.w, t.h);
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, depth ? kDepthFormat : GL_RGBA8, GLsizei(t.w), GLsizei(t.h));
    GL_CHECK("target storage");
    GlTarget* p = &(targets_[key] = t);
    // new targets start cleared, like the console's memory the title has not drawn in yet
    if (BindFramebuffer(depth ? nullptr : p, depth ? p : nullptr)) {
        SetScissor(false);
        if (depth) {
            glDepthMask(GL_TRUE);
            glStencilMask(0xFF);
            glClearBufferfi(GL_DEPTH_STENCIL, 0, kDepthFar, 0);
        } else {
            const GLfloat zero[4] = {};
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearBufferfv(GL_COLOR, 0, zero);
        }
    }
    return p;
}

// the framebuffer of a colour and a depth target, either may be missing
bool GlesBackend::BindFramebuffer(GlTarget* rt, GlTarget* ds) {
    const uint64_t key = uint64_t(rt ? rt->tex : 0) << 32 | (ds ? ds->tex : 0);
    GLuint fbo;
    auto it = framebuffers_.find(key);
    if (it != framebuffers_.end()) {
        fbo = it->second;
    } else {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (rt) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
        if (ds) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds->tex, 0);
        const GLenum buf = rt ? GL_COLOR_ATTACHMENT0 : GL_NONE;
        glDrawBuffers(1, &buf);
        glReadBuffer(buf);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_FIRST(8, "gles: framebuffer incomplete (0x%04X)", status);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            bound_fbo_ = ~0u;
            return false;
        }
        framebuffers_[key] = fbo;
        bound_fbo_ = fbo;
        return true;
    }
    if (bound_fbo_ != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        bound_fbo_ = fbo;
    }
    return true;
}

void GlesBackend::SetScissor(bool on) {
    if (on) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
}

// ---- shaders --------------------------------------------------------------------------------------------------------

// HLSL to a compiled GL shader, once per distinct source
GLuint GlesBackend::Shader(const std::string& hlsl, spirv::Stage stage, uint64_t& hash) {
    hash = Hash(hlsl.data(), hlsl.size(), uint64_t(stage) + 1);
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) return it->second;
    PerfTimer compile_timer(perf_.compile_ms);
    perf_.shaders++;
    gles::TranslateOptions o;
    o.clip_control = clip_control_;
    o.noperspective = noperspective_;
    std::string glsl, err;
    GLuint sh = 0;
    if (!gles::HlslToGlsl(hlsl, stage, o, glsl, err)) {
        LOGE("gles: shader translation failed: %s", err.c_str());
        LOGE("%s", hlsl.c_str());
    } else {
        sh = glCreateShader(stage == spirv::Stage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const char* src = glsl.c_str();
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048] = {};
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            LOGE("gles: shader compile failed: %s", log);
            LOGE("%s", glsl.c_str());
            glDeleteShader(sh);
            sh = 0;
        }
    }
    shaders_[hash] = sh;
    return sh;
}

GLuint GlesBackend::Program(uint64_t vh, uint64_t ph, GLuint vs, GLuint ps) {
    const uint64_t key = Hash(&ph, sizeof(ph), vh);
    auto it = programs_.find(key);
    if (it != programs_.end()) return it->second;
    PerfTimer pso_timer(perf_.pso_ms);
    perf_.psos++;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, ps);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("gles: program link failed: %s", log);
        glDeleteProgram(p);
        p = 0;
    }
    programs_[key] = p;
    return p;
}

// ---- textures -------------------------------------------------------------------------------------------------------

void GlesBackend::UploadTexture(GlTex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* guest,
                                const uint32_t* palette) {
    PerfTimer tex_timer(perf_.tex_ms);
    perf_.texups++;
    const uint32_t faces = d.cube ? 6 : 1;
    glBindTexture(tx.target, tx.tex);
    std::vector<uint8_t> px, conv;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, guest, palette, px, pitch);
            const uint32_t w = std::max(1u, tx.pad_w >> l), h = std::max(1u, tx.pad_h >> l);
            const uint32_t z = std::max(1u, d.depth >> l);
            const std::vector<uint8_t>* src = &px;
            uint32_t row_bytes, rows;
            if (tx.decode_bc) {
                DecodeBc(fi.host, px, w, h, conv);
                src = &conv;
                row_bytes = ((w + 3) & ~3u) * 4;
                rows = (h + 3) & ~3u;
                pitch = row_bytes;
            } else if (tx.compressed) {
                row_bytes = ((w + 3) / 4) * BlockBytes(fi.host);
                rows = (h + 3) / 4;
            } else {
                row_bytes = w * TexelBytes(fi.host);
                rows = h;
            }
            // rows tightly packed; a short or missing source row reads as zero
            staging_.assign(size_t(row_bytes) * rows * z, 0);
            const size_t src_rows = pitch ? src->size() / pitch / z : 0;
            for (uint32_t zi = 0; zi < z; zi++)
                for (uint32_t y = 0; y < rows && y < src_rows; y++)
                    std::memcpy(&staging_[(size_t(zi) * rows + y) * row_bytes],
                                &(*src)[(size_t(zi) * src_rows + y) * pitch], std::min<size_t>(pitch, row_bytes));
            const void* data = staging_.data();
            std::vector<float> wide;
            if (tx.r16_as_float) {  // R16 without GL_EXT_texture_norm16: as floats
                wide.resize(size_t(w) * h * z);
                for (size_t i = 0; i < wide.size(); i++) {
                    uint16_t v;
                    std::memcpy(&v, &staging_[i * 2], 2);
                    wide[i] = float(v) / 65535.0f;
                }
                data = wide.data();
            }
            const GLenum face = d.cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : tx.target;
            if (tx.compressed) {
                const GLsizei bytes = GLsizei(size_t(row_bytes) * rows * z);
                if (tx.target == GL_TEXTURE_3D)
                    glCompressedTexSubImage3D(face, GLint(l), 0, 0, 0, GLsizei(w), GLsizei(h), GLsizei(z), tx.internal,
                                              bytes, data);
                else
                    glCompressedTexSubImage2D(face, GLint(l), 0, 0, GLsizei(w), GLsizei(h), tx.internal, bytes, data);
            } else {
                GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
                if (tx.internal == GL_R32F) {
                    format = GL_RED;
                    type = GL_FLOAT;
                } else if (tx.internal == kR16) {
                    format = GL_RED;
                    type = GL_UNSIGNED_SHORT;
                }
                if (tx.target == GL_TEXTURE_3D)
                    glTexSubImage3D(face, GLint(l), 0, 0, 0, GLsizei(w), GLsizei(h), GLsizei(z), format, type, data);
                else
                    glTexSubImage2D(face, GLint(l), 0, 0, GLsizei(w), GLsizei(h), format, type, data);
            }
        }
    }
    GL_CHECK("texture upload");
}

// the stage's texture: a render target the title drew, or guest memory uploaded as is; a dummy when neither
bool GlesBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.target[stage] = GL_TEXTURE_2D;
    stages.tex[stage] = dummy_[kTex2D];
    stages.unfilterable[stage] = false;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return false;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    stages.target[stage] = TexTarget(TexKind(info.kind));
    stages.tex[stage] = dummy_[info.kind];

    // rendered by the GPU: sample the target itself
    auto tit = targets_.find(uint64_t(d.data) << 1 | (fi.depth ? 1 : 0));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed && tit->second.gw == d.width &&
        tit->second.gh == d.height) {
        GlTarget& t = tit->second;
        if (&t == bound_rt_ || &t == bound_ds_) {
            LOG_FIRST(2, "gles: a draw samples the target it renders to; reading a dummy");
            return false;
        }
        if (fi.depth) info.shadow = ShadowDepthKind(d.format);
        else info.flags |= kTexTarget;
        stages.tex[stage] = t.tex;
        return true;
    }
    if (fi.host == Host::None || fi.depth) return false;

    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    GlTex& tx = textures_[key];
    const bool same = tx.tex && tx.d.width == d.width && tx.d.height == d.height && tx.d.depth == d.depth &&
                      tx.d.levels == d.levels && tx.d.format == d.format && tx.d.cube == d.cube &&
                      tx.d.linear == d.linear;
    if (!same) {
        if (tx.tex) glDeleteTextures(1, &tx.tex);
        tx = GlTex{};
        tx.d = d;
        tx.target = stages.target[stage];
        tx.decode_bc = IsBc(fi.host) && !s3tc_;
        tx.compressed = IsBc(fi.host) && s3tc_;
        bool swizzle = false;
        switch (fi.host) {
            case Host::BC1: tx.internal = tx.decode_bc ? GL_RGBA8 : kDxt1; swizzle = tx.decode_bc; break;
            case Host::BC2: tx.internal = tx.decode_bc ? GL_RGBA8 : kDxt3; swizzle = tx.decode_bc; break;
            case Host::BC3: tx.internal = tx.decode_bc ? GL_RGBA8 : kDxt5; swizzle = tx.decode_bc; break;
            case Host::R32F: tx.internal = GL_R32F; tx.unfilterable = !float_linear_; break;
            case Host::R16Unorm:
                tx.internal = norm16_ ? kR16 : GL_R32F;
                tx.r16_as_float = !norm16_;
                tx.unfilterable = !norm16_ && !float_linear_;
                break;
            default: tx.internal = GL_RGBA8; swizzle = true; break;
        }
        tx.pad_w = d.width;
        tx.pad_h = d.height;
        tx.levels = std::max(1u, d.levels);
        // a block compressed level that is not a whole number of blocks is kept as one padded level
        if (IsBc(fi.host) && ((d.width & 3) || (d.height & 3))) {
            tx.pad_w = (d.width + 3) & ~3u;
            tx.pad_h = (d.height + 3) & ~3u;
            tx.levels = 1;
        }
        // GL refuses more levels than the size allows
        const uint32_t largest = std::max({tx.pad_w, tx.pad_h, d.depth});
        uint32_t most = 1;
        while ((largest >> most) > 0) most++;
        tx.levels = std::min(tx.levels, most);
        glGenTextures(1, &tx.tex);
        glBindTexture(tx.target, tx.tex);
        if (tx.target == GL_TEXTURE_3D)
            glTexStorage3D(tx.target, GLsizei(tx.levels), tx.internal, GLsizei(tx.pad_w), GLsizei(tx.pad_h),
                           GLsizei(std::max(1u, d.depth)));
        else
            glTexStorage2D(tx.target, GLsizei(tx.levels), tx.internal, GLsizei(tx.pad_w), GLsizei(tx.pad_h));
        if (swizzle) {
            // the guest's BGRA bytes went in as RGBA: red and blue come back in place when sampled
            glTexParameteri(tx.target, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(tx.target, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }
        if (glGetError() != GL_NO_ERROR) {
            LOG_FIRST(8, "gles: texture storage %ux%ux%u, %u levels, format 0x%04X failed", tx.pad_w, tx.pad_h, d.depth,
                      tx.levels, tx.internal);
            glDeleteTextures(1, &tx.tex);
            textures_.erase(key);
            return false;
        }
        tx.sig = ~0ull;
    }
    // the bytes are checked once a frame
    if (tx.frame != presents_) {
        tx.frame = presents_;
        const uint64_t sig = MemSig(d.data, TotalSize(d));
        if (sig != tx.sig || palette.hash != tx.palette) {
            tx.sig = sig;
            tx.palette = palette.hash;
            UploadTexture(tx, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), colours);
        }
    }
    stages.tex[stage] = tx.tex;
    stages.unfilterable[stage] = tx.unfilterable;
    return true;
}

GLuint GlesBackend::Sampler(const State& s, int i, const DrawTextures& stages) {
    const auto& t = s.tss[size_t(i)];
    const uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    const bool shadow = stages.info[i].shadow != 0;
    const bool point = stages.unfilterable[i] && !shadow;  // a float format GL cannot filter here
    struct Key {
        GLint mag, min, wrap[3];
        float min_lod, max_lod, aniso;
        GLint compare, func;
        float border[4];
    } k{};
    k.mag = lin(mag) && !point ? GL_LINEAR : GL_NEAREST;
    const bool lmin = lin(min) && !point;
    const bool lmip = mip == kTexfLinear && !point && !shadow;
    k.min = lmip ? (lmin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                 : (lmin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
    k.wrap[0] = GLint(Address(t[kTssAddressU]));
    k.wrap[1] = GLint(Address(t[kTssAddressV]));
    k.wrap[2] = GLint(Address(t[kTssAddressW]));
    // no mip filter: the level MaxMipLevel names, as on Vulkan
    k.min_lod = float(t[kTssMaxMipLevel]);
    k.max_lod = mip ? 1000.0f : k.min_lod;
    const bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    k.aniso = anisotropic && anisotropic_ && !shadow && !point
                  ? std::min(float(std::max<uint32_t>(t[kTssMaxAnisotropy], 1)), max_anisotropy_)
                  : 1.0f;
    Color(t[kTssBorderColor], k.border);
    if (shadow) {
        // the console tests texel OP r; with depth stored reversed (g_reversed_depth) the same OP holds here
        k.compare = GL_COMPARE_REF_TO_TEXTURE;
        k.func = GLint(Cmp(s.rs[kRsShadowFunc]));
    } else {
        k.compare = GL_NONE;
    }
    const uint64_t key = Hash(&k, sizeof(k));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    GLuint sm = 0;
    glGenSamplers(1, &sm);
    glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, k.mag);
    glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, k.min);
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, k.wrap[0]);
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, k.wrap[1]);
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_R, k.wrap[2]);
    glSamplerParameterf(sm, GL_TEXTURE_MIN_LOD, k.min_lod);
    glSamplerParameterf(sm, GL_TEXTURE_MAX_LOD, k.max_lod);
    glSamplerParameterfv(sm, GL_TEXTURE_BORDER_COLOR, k.border);
    if (anisotropic_) glSamplerParameterf(sm, kMaxAnisotropy, k.aniso);
    glSamplerParameteri(sm, GL_TEXTURE_COMPARE_MODE, k.compare);
    if (shadow) glSamplerParameteri(sm, GL_TEXTURE_COMPARE_FUNC, k.func);
    GL_CHECK("sampler");
    samplers_[key] = sm;
    return sm;
}

// ---- draw -----------------------------------------------------------------------------------------------------------

void GlesBackend::ApplyState(const State& s, const Assembled& a, bool has_rt, bool has_ds) {
    // blending and the colour mask
    if (has_rt && SimpleState(s, kMBlendEnable)) {
        glEnable(GL_BLEND);
        const uint32_t src = SimpleState(s, kMSrcBlend), dst = SimpleState(s, kMDestBlend);
        glBlendFunc(Blend(src, false), Blend(dst, true));
        glBlendEquation(BlendOp(SimpleState(s, kMBlendOp)));
        float bf[4];
        Color(SimpleState(s, kMBlendColor), bf);
        glBlendColor(bf[0], bf[1], bf[2], bf[3]);
    } else {
        glDisable(GL_BLEND);
    }
    const uint32_t cw = SimpleState(s, kMColorWriteEnable);  // console ARGB bytes
    glColorMask((cw >> 16) & 1, (cw >> 8) & 1, cw & 1, (cw >> 24) & 1);

    // depth and stencil
    bool zen = false, sten = false;
    float zslope = 0;
    if (has_ds) {
        zen = s.rs[kRsZEnable] != 0;
        sten = s.rs[kRsStencilEnable] != 0;
        if (OffsetsPolygons(s, a.topology)) zslope = -Bits(SimpleState(s, kMSlopeScale));  // toward the camera is up
    }
    if (zen) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(ReversedCmp(SimpleState(s, kMZFunc)));
        glDepthMask(SimpleState(s, kMZWriteEnable) ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    if (sten) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(Cmp(SimpleState(s, kMStencilFunc)), GLint(SimpleState(s, kMStencilRef) & 0xFF),
                      SimpleState(s, kMStencilMask) & 0xFF);
        glStencilMask(SimpleState(s, kMStencilWriteMask) & 0xFF);
        glStencilOp(StencilOp(s.rs[kRsStencilFail]), StencilOp(SimpleState(s, kMStencilZFail)),
                    StencilOp(SimpleState(s, kMStencilPass)));
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (zslope != 0) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(zslope, 0);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // culling: Vulkan's front face, seen mirrored (the rows are stored top first, GL counts them from the bottom)
    const uint32_t cull = s.rs[kRsCullMode];
    if (cull != 0 && a.topology == Topology::Triangles) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glFrontFace(cull == kCullCw ? GL_CW : GL_CCW);
    if (s.rs[kRsFillMode] == kFillWireframe) LOG_FIRST(1, "gles: wireframe is drawn solid (ES has no polygon mode)");
}

void GlesBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (!MakeCurrent()) return;
    PerfTimer draw_timer(perf_.draw_ms);
    perf_.draws++;
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    if (a.topology == Topology::Points) {
        LOG_FIRST(1, "gles: point lists are not drawn yet");
        return;
    }

    GlTarget* rt = EnsureTarget(s.rt, false);
    GlTarget* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    bound_rt_ = rt;
    bound_ds_ = ds;

    // shaders
    DrawShaders sh;
    static_cast<DrawKind&>(sh) = ClassifyDraw(s, a, count);
    sh.vkey = VertexKey(s, a, sh);
    // the HLSL is only written for a shader this key has not made before: it is kilobytes of text per draw otherwise
    auto vit = shader_keys_.find(sh.vkey);
    if (vit != shader_keys_.end()) {
        sh.vs = vit->second.shader;
        sh.vh = vit->second.hash;
    } else {
        sh.vs = Shader(VertexHlsl(s, a, sh), spirv::Stage::Vertex, sh.vh);
        shader_keys_[sh.vkey] = {sh.vs, sh.vh};
    }

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    {
        PerfTimer bind_timer(perf_.bind_ms);
        for (int i = 0; i < 4; i++) BindTexture(s, i, stages);
    }
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    auto pit = shader_keys_.find(sh.pkey ^ kPixelKeySalt);  // pixel keys share the map, the salt keeps them apart
    if (pit != shader_keys_.end()) {
        sh.ps = pit->second.shader;
        sh.ph = pit->second.hash;
    } else {
        sh.ps = Shader(PixelHlsl(pk), spirv::Stage::Pixel, sh.ph);
        shader_keys_[sh.pkey ^ kPixelKeySalt] = {sh.ps, sh.ph};
    }
    if (!sh.vs || !sh.ps) return;
    const GLuint program = Program(sh.vh, sh.ph, sh.vs, sh.ps);
    if (!program) return;

    // the rings: a frame that filled them waits for the GPU and starts them over
    size_t vertex_bytes = 0;
    for (uint32_t i = 0; i < a.buffer_count; i++) vertex_bytes += a.buffers[i].bytes + 16;
    const size_t index_bytes = a.index_count * (a.index16 ? 2 : 4);
    if (!vertices_.Fits(vertex_bytes) || !indices_.Fits(index_bytes + 4) ||
        !uniforms_.Fits(sizeof(s.vsc) + sizeof(DrawConstants) + 2 * size_t(ubo_align_)))
        WaitAll();

    // constants, each uploaded again only when it changed since the last draw
    GuestExtent size;
    SurfaceSize(rt ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillDrawConstants(s, sh, a.topology, size, 0.5f * (sx_ + sy_), c);
    static_assert(sizeof(s.vsc) == sizeof(last_vsc_), "vertex constants: 192 float4");
    if (last_vsc_at_ == ~size_t(0) || std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(last_vsc_)) != 0) {
        std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(last_vsc_));
        last_vsc_at_ = uniforms_.Put(s.vsc.data(), sizeof(s.vsc));
    }
    if (last_dc_at_ == ~size_t(0) || std::memcmp(&last_dc_, &c, sizeof(c)) != 0) {
        last_dc_ = c;
        last_dc_at_ = uniforms_.Put(&c, sizeof(c));
    }

    // geometry: a buffer with BGRA colours has their bytes swapped to RGBA (ES has no BGRA vertex format)
    std::array<size_t, 16> vb_at{};
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        const uint8_t* src = b.src;
        bool swap = false;
        for (int r = 0; r < 16; r++)
            if (((a.regs >> r) & 1) && a.elements[size_t(r)].buffer == i && VertexFormat(a.elements[size_t(r)].format).bgra)
                swap = true;
        if (swap && b.stride) {
            staging_.assign(b.src, b.src + b.bytes);
            for (int r = 0; r < 16; r++) {
                const VertexElement& e = a.elements[size_t(r)];
                if (!((a.regs >> r) & 1) || e.buffer != i || !VertexFormat(e.format).bgra) continue;
                for (size_t at = e.offset; at + 4 <= staging_.size(); at += b.stride) std::swap(staging_[at], staging_[at + 2]);
            }
            src = staging_.data();
        }
        vb_at[i] = vertices_.Put(src, b.bytes);
    }
    size_t ib_at = 0;
    if (a.index_count) ib_at = indices_.Put(a.index_src, index_bytes);

    // record
    if (!BindFramebuffer(rt, ds)) return;
    glUseProgram(program);
    const GLsizei W = GLsizei(rt ? rt->w : ds->w), H = GLsizei(rt ? rt->h : ds->h);
    glViewport(0, 0, W, H);  // the guest viewport is in the shader
    int32_t x0 = int32_t(s.viewport.x), y0 = int32_t(s.viewport.y);
    int32_t x1 = int32_t(s.viewport.x + s.viewport.w), y1 = int32_t(s.viewport.y + s.viewport.h);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0]));
        y0 = std::max(y0, int32_t(s.scissors[1]));
        x1 = std::min(x1, int32_t(s.scissors[2]));
        y1 = std::min(y1, int32_t(s.scissors[3]));
    }
    {
        const GlTarget* tg = rt ? rt : ds;  // the rect is in console pixels, the target in host pixels
        const float kx = float(tg->w) / float(tg->gw), ky = float(tg->h) / float(tg->gh);
        x0 = int32_t(x0 * kx);
        y0 = int32_t(y0 * ky);
        x1 = int32_t(std::lround(x1 * kx));
        y1 = int32_t(std::lround(y1 * ky));
    }
    x0 = std::clamp(x0, 0, int32_t(W));
    y0 = std::clamp(y0, 0, int32_t(H));
    x1 = std::clamp(x1, x0, int32_t(W));
    y1 = std::clamp(y1, y0, int32_t(H));
    SetScissor(true);
    glScissor(x0, y0, x1 - x0, y1 - y0);  // rows top first: the console's y is the row
    ApplyState(s, a, rt != nullptr, ds != nullptr);

    glBindBufferRange(GL_UNIFORM_BUFFER, 0, uniforms_.buffer(), GLintptr(last_vsc_at_), sizeof(s.vsc));
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, uniforms_.buffer(), GLintptr(last_dc_at_), sizeof(DrawConstants));
    for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + GLenum(i));
        glBindTexture(stages.target[i], stages.tex[i]);
        glBindSampler(GLuint(i), Sampler(s, i, stages));
    }

    glBindBuffer(GL_ARRAY_BUFFER, vertices_.buffer());
    uint16_t enabled = 0;
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        const VertexElement& e = a.elements[size_t(r)];
        const AttribFormat f = VertexFormat(e.format);
        const GLsizei stride = GLsizei(a.buffers[e.buffer].stride);
        const void* at = reinterpret_cast<const void*>(vb_at[e.buffer] + e.offset);
        if (f.integer) glVertexAttribIPointer(GLuint(r), f.size, f.type, stride, at);
        else glVertexAttribPointer(GLuint(r), f.size, f.type, f.normalized ? GL_TRUE : GL_FALSE, stride, at);
        enabled |= uint16_t(1u << r);
    }
    for (int r = 0; r < 16; r++) {
        const uint16_t bit = uint16_t(1u << r);
        if ((enabled & bit) && !(enabled_attribs_ & bit)) glEnableVertexAttribArray(GLuint(r));
        if (!(enabled & bit) && (enabled_attribs_ & bit)) glDisableVertexAttribArray(GLuint(r));
    }
    enabled_attribs_ = enabled;

    const GLenum mode = a.topology == Topology::Lines ? (a.strip ? GL_LINE_STRIP : GL_LINES)
                                                      : (a.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES);
    if (a.index_count) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_.buffer());
        glDrawElementsBaseVertex(mode, GLsizei(a.index_count), a.index16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                 reinterpret_cast<const void*>(ib_at), a.base_vertex);
    } else {
        glDrawArrays(mode, 0, GLsizei(a.draw_count));
    }
    GL_CHECK("draw");
}

// ---- clear, copy, present -------------------------------------------------------------------------------------------

void GlesBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                        uint32_t stencil) {
    if (!MakeCurrent()) return;
    GlTarget* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    GlTarget* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    if (!rt && !ds) return;
    const GlTarget* any = rt ? rt : ds;
    const float kx = float(any->w) / float(any->gw), ky = float(any->h) / float(any->gh);  // console pixels to host
    struct Box {
        GLint x, y;
        GLsizei w, h;
    };
    std::vector<Box> boxes;
    auto add = [&](float l, float t, float r, float b) {
        const int32_t x0 = std::clamp(int32_t(l * kx), 0, int32_t(any->w)), y0 = std::clamp(int32_t(t * ky), 0, int32_t(any->h));
        const int32_t x1 = std::clamp(int32_t(std::lround(r * kx)), x0, int32_t(any->w));
        const int32_t y1 = std::clamp(int32_t(std::lround(b * ky)), y0, int32_t(any->h));
        boxes.push_back({x0, y0, x1 - x0, y1 - y0});
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(float(r[0]), float(r[1]), float(r[2]), float(r[3]));
        }
    } else {
        const auto& v = s.viewport;
        add(float(v.x), float(v.y), float(v.x + v.w), float(v.y + v.h));
    }
    float rgba[4];
    Color(color, rgba);
    const float depth = 1 - std::clamp(z, 0.0f, 1.0f);
    // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7
    const GLboolean r = (flags >> 4) & 1, g = (flags >> 5) & 1, b = (flags >> 6) & 1, al = (flags >> 7) & 1;
    auto clear_into = [&](GlTarget* c, GlTarget* d) {
        if (!BindFramebuffer(c, d)) return;
        SetScissor(true);
        if (c) glColorMask(r, g, b, al);
        if (d) {
            glDepthMask(GL_TRUE);
            glStencilMask(0xFF);
        }
        for (const Box& bx : boxes) {
            glScissor(bx.x, bx.y, bx.w, bx.h);
            if (c) glClearBufferfv(GL_COLOR, 0, rgba);
            if (d) {
                if ((flags & kClearZ) && (flags & kClearStencil)) glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, GLint(stencil & 0xFF));
                else if (flags & kClearZ) glClearBufferfv(GL_DEPTH, 0, &depth);
                else {
                    const GLint st = GLint(stencil & 0xFF);
                    glClearBufferiv(GL_STENCIL, 0, &st);
                }
            }
        }
    };
    if (rt && ds && rt->w == ds->w && rt->h == ds->h) {
        clear_into(rt, ds);
    } else {
        if (rt) clear_into(rt, nullptr);
        if (ds) clear_into(nullptr, ds);
    }
    GL_CHECK("clear");
}

void GlesBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    if (!MakeCurrent()) return;
    const bool depth = Format(src.format).depth;
    GlTarget* st = EnsureTarget(src, depth);
    GlTarget* dt = EnsureTarget(dst, depth);
    if (!st || !dt) {
        CopySurfaceMemory(src, dst, count, src_rects, dst_points);
        return;
    }
    if (st == dt) return;
    const uint32_t n = count ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = 0, t = 0, r = int32_t(st->gw), b = int32_t(st->gh), dx = 0, dy = 0;  // console pixels
        if (count && src_rects) {
            const uint32_t at = src_rects + i * 16;
            l = int32_t(kernel::R32(at));
            t = int32_t(kernel::R32(at + 4));
            r = int32_t(kernel::R32(at + 8));
            b = int32_t(kernel::R32(at + 12));
            dx = l;
            dy = t;
            if (dst_points) {
                dx = int32_t(kernel::R32(dst_points + i * 8));
                dy = int32_t(kernel::R32(dst_points + i * 8 + 4));
            }
        }
        const float skx = float(st->w) / float(st->gw), sky = float(st->h) / float(st->gh);
        const float dkx = float(dt->w) / float(dt->gw), dky = float(dt->h) / float(dt->gh);
        l = std::clamp(int32_t(l * skx), 0, int32_t(st->w));
        t = std::clamp(int32_t(t * sky), 0, int32_t(st->h));
        r = std::clamp(int32_t(std::lround(r * skx)), l, int32_t(st->w));
        b = std::clamp(int32_t(std::lround(b * sky)), t, int32_t(st->h));
        dx = std::clamp(int32_t(dx * dkx), 0, int32_t(dt->w));
        dy = std::clamp(int32_t(dy * dky), 0, int32_t(dt->h));
        const int32_t cw = std::min(r - l, int32_t(dt->w) - dx), ch = std::min(b - t, int32_t(dt->h) - dy);
        if (cw <= 0 || ch <= 0) continue;
        // both hold their rows top first: the console's rects are the rows
        glCopyImageSubData(st->tex, GL_TEXTURE_2D, 0, l, t, 0, dt->tex, GL_TEXTURE_2D, 0, dx, dy, 0, cw, ch, 1);
    }
    GL_CHECK("copy");
}

// The render scale: a change remakes the targets at the new size; the title draws them again
void GlesBackend::ApplyScale() {
    float nx, ny;
    const uint32_t w = surf_w_ > 0 ? uint32_t(surf_w_) : guest_w_, h = surf_h_ > 0 ? uint32_t(surf_h_) : guest_h_;
    RenderScale(w, h, guest_w_, guest_h_, nx, ny);
    if (std::fabs(nx - sx_) < 1e-3f && std::fabs(ny - sy_) < 1e-3f) return;
    LOGI("gles: resolution %u%%: %.0fx%.0f for the %ux%u frame", GetResolutionPercent(), nx * guest_w_, ny * guest_h_,
         guest_w_, guest_h_);
    bound_rt_ = bound_ds_ = nullptr;
    for (auto& entry : targets_) ReleaseTarget(entry.second);
    targets_.clear();
    sx_ = nx;
    sy_ = ny;
}

void GlesBackend::Present(const Surface& front) {
    if (!MakeCurrent()) return;
    GlTarget* t = EnsureTarget(front, false);
    if (surface_ == EGL_NO_SURFACE) {
        // the app is in the background: the frame is skipped (at a calm pace, the game is not visible) until the
        // activity has a new window, then a surface is made from it
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ANativeWindow* w = CurrentAndroidWindow();
        if (w && CreateWindowSurface(w)) {
            LOGI("gles: surface made again, %dx%d", surf_w_, surf_h_);
            MakeCurrent();
        }
        EndFrame();
        presents_++;
        NextFrame();
        return;
    }
    eglQuerySurface(dpy_, surface_, EGL_WIDTH, &surf_w_);
    eglQuerySurface(dpy_, surface_, EGL_HEIGHT, &surf_h_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    bound_fbo_ = ~0u;
    SetScissor(false);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (t) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, present_fbo_);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        // the target's first row is the picture's top, the window's first row its bottom: the blit flips
        const PictureRect pic = PictureOnScreen(uint32_t(surf_w_), uint32_t(surf_h_));
        const GLint top = surf_h_ - pic.y, bottom = surf_h_ - pic.y - pic.h;
        glBlitFramebuffer(0, 0, GLint(t->w), GLint(t->h), pic.x, top, pic.x + pic.w, bottom, GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }
    GL_CHECK("present");
    EndFrame();
    bool lost = false;
    {
        PerfTimer timer(perf_.present_ms);
        if (!eglSwapBuffers(dpy_, surface_)) {
            const EGLint e = eglGetError();
            LOGI("gles: eglSwapBuffers failed (0x%X)", e);
            lost = e == EGL_BAD_SURFACE || e == EGL_BAD_NATIVE_WINDOW || e == EGL_CONTEXT_LOST;
        }
    }
    if (lost) LoseSurface();
    presents_++;
    perf_.frames++;
    pacer_.Pace();
    NextFrame();
    ApplyScale();
    LogPerf(perf_, perf_start_);
}

}  // namespace

std::unique_ptr<Backend> CreateGlesBackend() { return std::make_unique<GlesBackend>(); }

}  // namespace gpu

#endif  // __ANDROID__
