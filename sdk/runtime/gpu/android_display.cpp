// The Android renderers' settings and what both share at present time (android_platform.h), and the choice between them.
//
// Auto starts Vulkan and falls back to OpenGL ES when Vulkan does not start, or its GPU is on kVulkanBlocklist, or the last
// start on Vulkan never got as far as kConfirmFrames frames (the driver crashed or hung): a marker file is written before
// Vulkan starts and removed once it has drawn that many, so a start that finds it in place uses OpenGL ES. The app removes
// the marker when the player changes the renderer setting.
#if defined(__ANDROID__)

#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/log.h"
#include "gpu/android_platform.h"
#include "gpu/backend.h"
#include "gpu/host_common.h"

namespace gpu {

std::unique_ptr<Backend> CreateVulkanBackend();
std::unique_ptr<Backend> CreateGlesBackend();
bool g_vulkan_auto = false;  // read by the Vulkan backend: it refuses blocklisted GPUs only when the player chose Auto

namespace {

ANativeWindow* g_window = nullptr;
ANativeWindow* (*g_window_provider)() = nullptr;
std::atomic<int> g_display_mode{kDisplayFill};
constexpr float kMaxFillAspect = 2.4f;  // 21:9 phones are 2.33
std::atomic<uint32_t> g_frame_limit{0};
std::atomic<float> g_fps{0};
std::atomic<uint32_t> g_resolution_percent{100};
std::atomic<int> g_renderer_choice{kRendererAuto};
std::atomic<int> g_active_renderer{kRendererAuto};
std::string g_marker;  // the boot check's file, empty for none
constexpr uint64_t kConfirmFrames = 120;

// Vulkan drivers that start but draw wrongly or crash, by a piece of VkPhysicalDeviceProperties::deviceName. Auto
// goes to OpenGL ES on these. Add what players report.
const char* const kVulkanBlocklist[] = {
    nullptr,
};

bool MarkerPresent() {
    if (g_marker.empty()) return false;
    FILE* f = std::fopen(g_marker.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

void WriteMarker() {
    if (g_marker.empty()) return;
    if (FILE* f = std::fopen(g_marker.c_str(), "wb")) {
        std::fputs("vulkan start in progress\n", f);
        std::fclose(f);
    }
}

// frames presented since the renderer started: the boot check is confirmed at kConfirmFrames
std::atomic<uint64_t> g_frames{0};

}  // namespace

bool VulkanBlocked(const char* device_name) {
    if (!g_vulkan_auto || !device_name) return false;
    for (const char* bad : kVulkanBlocklist)
        if (bad && std::strstr(device_name, bad)) {
            LOGI("renderer: %s is on the Vulkan blocklist", device_name);
            return true;
        }
    return false;
}

namespace {

// the backend CreateBackend hands out: its Init picks the renderer and starts it
class Chooser final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        const int choice = g_renderer_choice.load();
        g_vulkan_auto = choice == kRendererAuto;
        bool try_vulkan = choice != kRendererGles;
        if (choice == kRendererAuto && MarkerPresent()) {
            LOGI("renderer: the last start on Vulkan did not get to %llu frames; using OpenGL ES",
                 (unsigned long long)kConfirmFrames);
            try_vulkan = false;
        }
        if (try_vulkan) {
            if (choice == kRendererAuto) WriteMarker();
            g_reversed_depth = true;
            auto vk = CreateVulkanBackend();
            if (vk->Init(w, h)) {
                LOGI("renderer: Vulkan");
                inner_ = std::move(vk);
                g_active_renderer = kRendererVulkan;
                return true;
            }
            if (choice == kRendererVulkan) return false;
            LOGI("renderer: Vulkan did not start; trying OpenGL ES");
            // the failed backend goes before GLES takes the window (a Vulkan surface may still hold it)
            vk.reset();
            if (!g_marker.empty()) std::remove(g_marker.c_str());
        }
        g_reversed_depth = true;
        auto gl = CreateGlesBackend();
        if (!gl->Init(w, h)) return false;
        LOGI("renderer: OpenGL ES");
        inner_ = std::move(gl);
        g_active_renderer = kRendererGles;
        if (choice == kRendererAuto && !g_marker.empty()) {
            // stays: a later Auto start goes straight to OpenGL ES until the player changes the setting
            if (FILE* f = std::fopen(g_marker.c_str(), "wb")) {
                std::fputs("opengl es\n", f);
                std::fclose(f);
            }
        }
        return true;
    }
    void Clear(const State& s, uint32_t n, const uint8_t* r, uint32_t f, uint32_t c, float z, uint32_t st) override {
        inner_->Clear(s, n, r, f, c, z, st);
    }
    void Draw(const State& s, uint32_t p, uint32_t st, uint32_t n, const uint8_t* i) override { inner_->Draw(s, p, st, n, i); }
    void Copy(const Surface& a, const Surface& b, uint32_t n, uint32_t r, uint32_t p) override { inner_->Copy(a, b, n, r, p); }
    void Present(const Surface& f) override { inner_->Present(f); }
    void Sync(uint32_t va, uint32_t n) override { inner_->Sync(va, n); }
    void SyncLate(uint32_t va) override { inner_->SyncLate(va); }
    bool DeliverLate(uint32_t va) override { return inner_->DeliverLate(va); }
    void RecordLate(uint32_t va) override { inner_->RecordLate(va); }
    void Flush() override { inner_->Flush(); }
    void SetGamma(const uint8_t* r) override { inner_->SetGamma(r); }
    void ClearReport() override { inner_->ClearReport(); }
    void GetReport(uint32_t r) override { inner_->GetReport(r); }
    uint32_t Fence() override { return inner_->Fence(); }
    bool FencePending(uint32_t f) override { return inner_->FencePending(f); }
    void WaitFence(uint32_t f) override { inner_->WaitFence(f); }
    bool Busy(uint32_t r) override { return inner_->Busy(r); }
    void WaitResource(uint32_t r) override { inner_->WaitResource(r); }
    void WaitGpuFrame(uint64_t n) override { inner_->WaitGpuFrame(n); }

private:
    std::unique_ptr<Backend> inner_;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<Chooser>(); }

void SetRendererChoice(int renderer) { g_renderer_choice = std::clamp(renderer, int(kRendererAuto), int(kRendererGles)); }
void SetRendererMarker(const char* path) { g_marker = path ? path : ""; }
int GetActiveRenderer() { return g_active_renderer; }

void SetAndroidWindow(ANativeWindow* window) { g_window = window; }
ANativeWindow* GetAndroidWindow() { return g_window; }
void SetAndroidWindowProvider(ANativeWindow* (*provider)()) { g_window_provider = provider; }
ANativeWindow* CurrentAndroidWindow() {
    ANativeWindow* w = g_window_provider ? g_window_provider() : nullptr;
    if (w) g_window = w;
    return w;
}
void SetDisplayMode(int mode) { g_display_mode = std::clamp(mode, int(kDisplayFill), int(kDisplay4x3)); }
int GetDisplayMode() { return g_display_mode; }
void SetResolutionPercent(uint32_t percent) { g_resolution_percent = std::clamp(percent, 10u, 200u); }
uint32_t GetResolutionPercent() { return g_resolution_percent; }
void SetFrameLimit(uint32_t fps) { g_frame_limit = fps ? std::clamp(fps, 10u, 1000u) : 0u; }
float GetFps() { return g_fps; }
float DisplayAspect(uint32_t width, uint32_t height) {
    const float screen = height ? float(width) / float(height) : 16.0f / 9.0f;
    switch (g_display_mode.load()) {
        case kDisplay4x3: return 4.0f / 3.0f;
        case kDisplay16x9: return 16.0f / 9.0f;
        default: return std::clamp(screen, 16.0f / 9.0f, kMaxFillAspect);
    }
}

namespace host {

// the console's picture is 16:9 (it reports a widescreen display, as BlitFrame in d3d12.cpp shows), or 4:3 when the
// player chose the original picture. On a wider screen the 16:9 one grows with the screen up to kMaxFillAspect (fill)
// or keeps its shape with bars at the sides; narrower, bars above and below
PictureRect PictureOnScreen(uint32_t screen_w, uint32_t screen_h) {
    const float aspect = DisplayAspect(screen_w, screen_h);
    const float vw = std::min(float(screen_w), float(screen_h) * aspect);
    const int32_t bw = int32_t(vw), bh = int32_t(vw / aspect);
    return {(int32_t(screen_w) - bw) / 2, (int32_t(screen_h) - bh) / 2, bw, bh};
}

// The render scale: the picture is drawn at the setting's percent of the screen's own pixels (100 is native), so a
// console pixel is sx by sy host pixels
void RenderScale(uint32_t screen_w, uint32_t screen_h, uint32_t guest_w, uint32_t guest_h, float& sx, float& sy) {
    const float aspect = DisplayAspect(screen_w, screen_h);
    const float vw = std::min(float(screen_w), float(screen_h) * aspect);
    const float vh = vw / aspect;
    const float p = float(g_resolution_percent.load()) / 100.0f;
    sx = vw * p / float(guest_w);
    sy = vh * p / float(guest_h);
}

// The deadline moves on by one period per frame, so the rate holds on average; a frame that ran late starts the count
// again from now
void FramePacer::Pace() {
    if (++g_frames == kConfirmFrames && !g_marker.empty() && g_active_renderer == kRendererVulkan) {
        std::remove(g_marker.c_str());  // Vulkan works here: the boot check is passed
        LOGI("renderer: Vulkan confirmed after %llu frames", (unsigned long long)kConfirmFrames);
    }
    const PerfClock::time_point now = PerfClock::now();
    frames_++;
    const double since = std::chrono::duration<double>(now - fps_start_).count();
    if (since >= 0.5) {
        g_fps = float(frames_ / since);
        frames_ = 0;
        fps_start_ = now;
    }
    const uint32_t limit = g_frame_limit.load();
    if (!limit) {
        next_ = {};
        return;
    }
    const auto period = std::chrono::duration_cast<PerfClock::duration>(std::chrono::nanoseconds(1000000000ull / limit));
    if (next_ == PerfClock::time_point{} || now > next_ + period) next_ = now;
    next_ += period;
    if (next_ > now) std::this_thread::sleep_until(next_);
}

}  // namespace host

}  // namespace gpu

#endif  // __ANDROID__
