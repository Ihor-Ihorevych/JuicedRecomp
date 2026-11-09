// The Android host: SDL3's activity gives a window, the game folder is on the phone's storage, and the boot is the one
// sdk/harness/main_test.cpp does on Linux. Replaces the SDK's app/ (which is the Windows host).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/native_window.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/android_platform.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace fs = std::filesystem;

namespace app {

namespace {
SDL_Window* g_window = nullptr;
uint32_t g_width = 1280, g_height = 720;
}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
void Save() {}
void LogSettings() {}
void LogSystem() {}
void ApplyDisplay() {}
Mode Desktop() { return {g_width, g_height}; }
Mode Output() { return Desktop(); }
float RenderScale(uint32_t) { return 1.0f; }
// the console reports a widescreen TV unless the player chose the original 4:3 picture
bool Widescreen() { return gpu::GetDisplayMode() != gpu::kDisplay4x3; }

// the app's own folder on the shared storage: logs, the console's disk and (by default) the game
fs::path ExeDir() {
    static const fs::path dir = [] {
        const char* p = SDL_GetAndroidExternalStoragePath();
        return fs::path(p ? p : "/sdcard/Android/data/io.github.juiced/files");
    }();
    return dir;
}
int Instance() { return 0; }
void* Window() { return g_window; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {}

}  // namespace app

namespace audio {
void SetPaused(bool paused);  // audio_aaudio.cpp
}

namespace {

// the game: a folder with default.xbe beside its files, <files>/game by default, or a path in <files>/game_path.txt
std::string FindGameFolder() {
    fs::path files = app::ExeDir();
    std::ifstream f(files / "game_path.txt");
    std::string path;
    if (f && std::getline(f, path) && !path.empty()) return path;
    return (files / "game").string();
}

bool OpenWindow() {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
    // the main thread keeps flushing the log and the game keeps its place while the app is in the background
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    // ... so the sound is stopped here, and started again when the app comes back (a watch sees the events without taking them)
    SDL_AddEventWatch(
        [](void*, SDL_Event* e) -> bool {
            if (e->type == SDL_EVENT_DID_ENTER_BACKGROUND) audio::SetPaused(true);
            if (e->type == SDL_EVENT_WILL_ENTER_FOREGROUND) audio::SetPaused(false);
            return true;
        },
        nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOGE("android: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    // SDL_WINDOW_VULKAN keeps SDL from making a GL context and surface: the renderer makes its own from the native
    // window, a Vulkan surface or an EGL one (gpu/gles.cpp)
    app::g_window = SDL_CreateWindow("Juiced", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN);
    if (!app::g_window) {
        LOGE("android: SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(app::g_window, &w, &h);
    if (w > 0 && h > 0) {
        app::g_width = uint32_t(w);
        app::g_height = uint32_t(h);
    }
    SDL_PropertiesID props = SDL_GetWindowProperties(app::g_window);
    auto* native = static_cast<ANativeWindow*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    if (!native) {
        LOGE("android: the window has no native surface");
        return false;
    }
    gpu::SetAndroidWindow(native);
    // SDL updates this property each time the activity gets a new window back from the background
    gpu::SetAndroidWindowProvider([]() -> ANativeWindow* {
        return static_cast<ANativeWindow*>(SDL_GetPointerProperty(SDL_GetWindowProperties(app::g_window),
                                                                  SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    });
    LOGI("android: window %dx%d", w, h);
    return true;
}

}  // namespace

int main(int, char**) {
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    LOGI("android: files %s", app::ExeDir().string().c_str());
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (!OpenWindow()) return 1;
    // the picture's shape (fill or fit) comes from the settings panel's saved choice: GameActivity sets it before this runs
    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_window);

    std::string game = FindGameFolder();
    std::ifstream f(fs::path(game) / IMAGE_XBE, std::ios::binary);
    std::vector<uint8_t> xbe((std::istreambuf_iterator<char>(f)), {});
    if (xbe.empty()) {
        LOGE("android: no %s in %s; copy the game's extracted files there (adb push) and start again", IMAGE_XBE,
             game.c_str());
        logx::Flush();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }
    LOGI("android: game %s, %zu byte xbe", game.c_str(), xbe.size());
    // the voices: <game>/lang/<code> holds a dub's files at the game's own paths, chosen in the launcher or the settings
    // (language.txt, "en" for the game's own)
    {
        std::ifstream lf(app::ExeDir() / "language.txt");
        std::string code;
        if (lf && std::getline(lf, code) && !code.empty() && code != "en") {
            fs::path dir = fs::path(game) / "lang" / code;
            std::error_code ec;
            if (fs::is_directory(dir, ec)) {
                kernel::SetGameOverlay(dir.string());
                LOGI("android: voices %s (%s)", code.c_str(), dir.string().c_str());
            } else {
                LOGW("android: voices %s chosen but %s is missing, playing the game's own", code.c_str(),
                     dir.string().c_str());
            }
        }
    }
    if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game, (app::ExeDir() / "hdd").string())) {
        logx::Flush();
        return 1;
    }
    // the guest runs on its own threads; this one keeps the window's events moving
    for (;;) {
        SDL_PumpEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        logx::Flush();
    }
}


// the on-screen controls (TouchControls.java) hand their pad state over from the UI thread: the button bits as the
// console controller reports them, A B X Y black white left and right trigger (0-255), then both sticks (-32768..32767)
extern "C" JNIEXPORT void JNICALL Java_io_github_juiced_TouchControls_nativeSetPad(
    JNIEnv*, jclass, jint buttons, jint a, jint b, jint x, jint y, jint black, jint white, jint lt, jint rt, jint lx,
    jint ly, jint rx, jint ry) {
    input::Pad pad;
    pad.connected = true;
    pad.buttons = uint16_t(buttons);
    const int analog[8] = {a, b, x, y, black, white, lt, rt};
    for (int i = 0; i < 8; i++) pad.analog[i] = uint8_t(analog[i]);
    pad.lx = int16_t(lx);
    pad.ly = int16_t(ly);
    pad.rx = int16_t(rx);
    pad.ry = int16_t(ry);
    input::SetTouchPad(pad);
}

// the settings panel's resolution slider (GameActivity.java), a percent of the screen's own pixels
extern "C" JNIEXPORT void JNICALL Java_io_github_juiced_GameActivity_nativeSetResolution(JNIEnv*, jclass,
                                                                                              jint percent) {
    gpu::SetResolutionPercent(uint32_t(std::max(percent, 10)));
}

// the settings panel's picture shape (GameActivity.java): 0 fill, 1 16:9 with bars, 2 the original 4:3; read when the
// game starts
extern "C" JNIEXPORT void JNICALL Java_io_github_juiced_GameActivity_nativeSetDisplayMode(JNIEnv*, jclass, jint mode) {
    gpu::SetDisplayMode(mode);
    static const char* const kNames[] = {"fill", "16:9", "4:3"};
    LOGI("android: display mode %s", kNames[gpu::GetDisplayMode()]);
}

// the settings panel's renderer (GameActivity.java): 0 Auto, 1 Vulkan, 2 OpenGL ES, and the file Auto keeps its boot check
// in; read when the game starts
extern "C" JNIEXPORT void JNICALL Java_io_github_juiced_GameActivity_nativeSetRenderer(JNIEnv* env, jclass,
                                                                                           jint renderer, jstring marker) {
    gpu::SetRendererChoice(renderer);
    const char* path = marker ? env->GetStringUTFChars(marker, nullptr) : nullptr;
    gpu::SetRendererMarker(path);
    if (path) env->ReleaseStringUTFChars(marker, path);
    static const char* const kNames[] = {"auto", "Vulkan", "OpenGL ES"};
    LOGI("android: renderer %s", kNames[std::clamp(int(renderer), 0, 2)]);
}

// the renderer the game started (1 Vulkan, 2 OpenGL ES), 0 before it has started one
extern "C" JNIEXPORT jint JNICALL Java_io_github_juiced_GameActivity_nativeGetActiveRenderer(JNIEnv*, jclass) {
    return gpu::GetActiveRenderer();
}

// the settings panel's frame rate limit, frames per second (0: none)
extern "C" JNIEXPORT void JNICALL Java_io_github_juiced_GameActivity_nativeSetFrameLimit(JNIEnv*, jclass, jint fps) {
    gpu::SetFrameLimit(uint32_t(std::max(fps, 0)));
}

// the frame rate counter (GameActivity.java) reads this twice a second
extern "C" JNIEXPORT jfloat JNICALL Java_io_github_juiced_GameActivity_nativeGetFps(JNIEnv*, jclass) {
    return gpu::GetFps();
}

// the activity is paused or resumed (GameActivity.java): the sound goes with it
extern "C" JNIEXPORT void JNICALL Java_io_github_juiced_GameActivity_nativeSetBackground(JNIEnv*, jclass,
                                                                                             jboolean background) {
    LOGI("android: %s", background ? "in the background" : "in front");
    audio::SetPaused(background != JNI_FALSE);
}
