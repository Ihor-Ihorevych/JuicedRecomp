#pragma once
// What the Android app hands the renderer (Vulkan or OpenGL ES) before the game boots.
#include <cstdint>

struct ANativeWindow;

namespace gpu {

// the surface the backend presents to; call before the title starts, and again with null when it goes away
void SetAndroidWindow(ANativeWindow* window);
ANativeWindow* GetAndroidWindow();

// asks the app for the window as it is now: the activity makes a new one each time it comes back from the background, and the
// backend remakes its surface (and swapchain) from it when the old one is lost
void SetAndroidWindowProvider(ANativeWindow* (*provider)());
// the provider's window as it is now (and remembered as GetAndroidWindow's), null when there is none
ANativeWindow* CurrentAndroidWindow();

// which renderer draws the game, read once when the game starts:
//   kRendererAuto     Vulkan, or OpenGL ES when Vulkan does not start or the GPU is on the list of drivers that break
//   kRendererVulkan   Vulkan only
//   kRendererGles     OpenGL ES 3.2 only
enum Renderer : int { kRendererAuto = 0, kRendererVulkan = 1, kRendererGles = 2 };
void SetRendererChoice(int renderer);
// Auto's boot check: a file in the app's own storage that marks a Vulkan start not yet confirmed (android_display.cpp);
// the app deletes it when the player changes the renderer
void SetRendererMarker(const char* path);
// the renderer that was started (kRendererVulkan or kRendererGles), kRendererAuto before the game has started one
int GetActiveRenderer();

// the picture's shape, read when the game starts:
//   kDisplayFill    widescreen that fills a wide screen (16:9 up to about 21:9)
//   kDisplay16x9    widescreen kept at 16:9, bars at the sides of a wider screen
//   kDisplay4x3     the console's original 4:3: the game is told of a 4:3 TV and draws its own 4:3 picture, bars at the sides
enum DisplayMode : int { kDisplayFill = 0, kDisplay16x9 = 1, kDisplay4x3 = 2 };
void SetDisplayMode(int mode);
int GetDisplayMode();

// the aspect ratio of the picture on a screen of this size: 4:3, 16:9, or in fill mode the screen's own up to about 21:9. The
// renderer shapes the picture by it and the game is told the same (src/game/hooks.cpp), so the picture is drawn at that aspect
float DisplayAspect(uint32_t width, uint32_t height);

// frames per second at most, 0 for no limit beyond the display's own pace; takes effect on the next frame
void SetFrameLimit(uint32_t fps);

// the frames per second presented, measured over the last half second
float GetFps();

// the resolution the picture is rendered at, as a percent of the screen's own pixels: 100 is native, less is faster; takes
// effect within a frame
void SetResolutionPercent(uint32_t percent);
uint32_t GetResolutionPercent();

}  // namespace gpu
