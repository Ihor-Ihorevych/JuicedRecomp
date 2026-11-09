# Juiced for Android arm64: libmain.so (SDL3 loads it), built by android/build-apk.sh.
#
# The runtime is the SDK's, as the Linux headless harness builds it (sdk/harness/build.sh): everything in
# sdk/runtime/*/*.cpp but the Windows host (app/, d3d12, XAudio2, texture dumper), with the recompiled game linked in
# (no X86_GAME_CODE: cpu/x86_ops.h's non-Windows branch), Vulkan or OpenGL ES 3.2 for the GPU and AAudio for sound.
cmake_minimum_required(VERSION 3.22)
project(juiced_android LANGUAGES C CXX)

set(REPO ${CMAKE_CURRENT_LIST_DIR}/..)
set(SDK ${REPO}/sdk)
set(GEN ${REPO}/generated/game)
if(NOT EXISTS ${GEN}/x86_func_mapping.cpp)
    message(FATAL_ERROR "No ${GEN}: run xbe_recomp first (android/build-apk.sh does)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# SDL3: window, gamepads, the Android activity glue
include(FetchContent)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
if(DEFINED ENV{SDL3_SOURCE_DIR})
    add_subdirectory($ENV{SDL3_SOURCE_DIR} SDL3 EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# glslang's HLSL front end compiles the generated shaders to SPIR-V at run time (gpu/spirv.cpp)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
if(EXISTS ${REPO}/third_party/glslang/CMakeLists.txt)
    set(GLSLANG_DIR ${REPO}/third_party/glslang)
    add_subdirectory(${GLSLANG_DIR} glslang EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 15.4.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glslang)
    set(GLSLANG_DIR ${glslang_SOURCE_DIR})
endif()

# SPIRV-Cross writes the SPIR-V out again as GLSL ES 3.20 for the OpenGL ES renderer (gpu/gles_shader.cpp)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
if(EXISTS ${REPO}/third_party/SPIRV-Cross/CMakeLists.txt)
    add_subdirectory(${REPO}/third_party/SPIRV-Cross spirv-cross EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
                         GIT_TAG vulkan-sdk-1.4.341.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(spirv_cross)
endif()

# the runtime
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${SDK}/runtime/*/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/runtime/app/")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/gpu/d3d12\\.cpp$")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/gpu/texturedumper\\.cpp$")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/audio/xaudio2\\.cpp$")
# the game's own host sources (src/common, src/game) and the recompiled image
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${REPO}/src/*/*.cpp)
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${GEN}/x86_recomp.*.cpp ${GEN}/x86_func_mapping.cpp)

add_library(main SHARED
    jni/app_android.cpp
    jni/audio_aaudio.cpp
    ${RUNTIME_SOURCES} ${GAME_SOURCES} ${GEN_SOURCES})
target_include_directories(main PRIVATE
    ${REPO}/src ${SDK}/runtime ${GEN} ${SDK}/harness/compat ${GLSLANG_DIR})
target_include_directories(main SYSTEM PRIVATE ${sdl3_SOURCE_DIR}/include)
target_compile_options(main PRIVATE -fno-strict-aliasing -fexceptions -w)
target_compile_definitions(main PRIVATE XBE_ANDROID=1 XBE_VERBOSE_LOG=1)
target_link_libraries(main PRIVATE
    SDL3::SDL3 glslang SPIRV glslang-default-resource-limits spirv-cross-glsl
    aaudio vulkan EGL GLESv3 android log)
