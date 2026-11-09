# Android port

An arm64 Android build of the recompiled game: the SDK's runtime with a Vulkan 1.1 renderer (`sdk/runtime/gpu/vulkan.cpp`,
HLSL to SPIR-V through glslang) and an OpenGL ES 3.2 one for phones whose Vulkan driver breaks (`sdk/runtime/gpu/gles.cpp`,
the same SPIR-V written out as GLSL ES by SPIRV-Cross), AAudio sound, SDL3 for the window and pads, and on-screen touch controls.
Status and known gaps: [`docs/android-vulkan.md`](../docs/android-vulkan.md).

The repository holds no game data. You need your own copy of the game.

## Build (Windows, Git Bash)

1. Build the host tools: `cmake -S tools-build -B tools-build/build -G Ninja && cmake --build tools-build/build`
   (MinGW works; no Visual Studio needed).
2. Extract your disc image so `game_files/default.xbe` exists, and copy `default.xbe` to `assets/`.
3. `bash android/build-apk.sh --recomp` builds `build/android/juiced.apk`; the game goes on the phone by hand
   (`adb push game_files /sdcard/Android/data/io.github.juiced/files/game`).
4. `bash android/build-apk.sh --selfpack` also builds `juiced-selfpack.apk`, which carries the game and unpacks it on the
   first start. It contains data and code derived from the game: keep it to your own phone.

Needs the Android SDK (platform 35, NDK 28.2.13676358, CMake 4.1.2) and a JDK; `--install` installs over adb.

## Settings

The top-left button opens the settings: resolution (25% to 100% of the screen's pixels), picture shape (fill the screen, or 16:9 with side bars; applies on the next start), renderer (Auto, Vulkan or OpenGL ES; applies on the next start), button opacity, visibility and style (game icons or Xbox A/B/X/Y), and links to the Telegram group and the source. Choices are saved in the app's preferences.

## Renderer

Settings > Graphics > Renderer picks what draws the game, from the next start:

- **Auto** (the default): Vulkan, or OpenGL ES when Vulkan does not start, the GPU is on the Vulkan blocklist
  (`kVulkanBlocklist` in `sdk/runtime/gpu/android_display.cpp`), or the last start on Vulkan never reached 120 frames
  (the driver crashed or hung). Once Auto has gone to OpenGL ES it stays there until the setting is changed.
- **Vulkan** or **OpenGL ES**: that one only.

The log (`logs/` in the app's folder) names the renderer that started (`renderer: Vulkan` / `renderer: OpenGL ES`).
