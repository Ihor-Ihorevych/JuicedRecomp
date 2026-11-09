# Android Vulkan backend: status

`sdk/runtime/gpu/vulkan.cpp` implements `gpu::Backend` with Vulkan 1.1 (render passes with one subpass, cached per attachment
formats, and framebuffers per target pair; vertex strides are part of the pipeline). The shaders are the HLSL `shaders.cpp` already writes for D3D12, compiled to SPIR-V at run time by
`gpu/spirv.cpp` (glslang's HLSL front end).

What is checked so far:

- `vulkan.cpp` and `spirv.cpp` compile for arm64 with NDK 28 (`-fsyntax-only`).
- 44 generated shaders (fixed function vertex shaders, pixel combiners, target / shadow / quad variants) compile to
  SPIR-V (`tools-build`, target `shader_probe`). SPIR-V validation is not run: glslang was built without spirv-tools.

Nothing has run on a device or an emulator yet.

## Resource layout (descriptor set 0, push descriptors)

| binding | HLSL | what |
|---|---|---|
| 0 | `b0` | vertex constants (192 x float4) |
| 1 | `b1` | `DrawConstants` |
| 2-5 | `t0`-`t3` | sampled images |
| 6-9 | `s0`-`s3` | samplers |

Vertex inputs: `ATTRn` is location n (`[[vk::location(n)]]` is added to the vertex input struct before compiling).

## Not done yet

| Area | State |
|---|---|
| Point lists | Not drawn. D3D12 expands points with a geometry shader (`PointHlsl`); Mali GPUs often lack geometry shaders. Needs a vertex-shader or instanced quad expansion. |
| Render target read-back (`Sync`, `SyncLate`) | No-op. Anything the title reads back from the GPU (CPU-side effects, screenshots) is wrong. |
| CPU writes to render targets (`LatchCpuWrite`, `UploadTarget`) | Not implemented. Text the title draws with the CPU into a target will not show. |
| Sampling the target being rendered to | A dummy texture is read; D3D12 keeps a feedback copy. |
| Partial-channel colour clears | Clears all four channels. |
| Occlusion queries | `GetReport` always reports 0x100 visible pixels. |
| Gamma ramp (`SetGamma`) | Ignored. |
| Render scale, extensions, texture pack, capture, perf log | Not ported. |
| Memory | One `vkAllocateMemory` per image; some drivers cap allocations near 4096. Needs a sub-allocator. |
| Texture upload | Hash of the guest bytes once per frame per texture, as D3D12 does; no sweep of unused textures. |
| BC textures | Decoded on the CPU when the GPU lacks `textureCompressionBC`. |
| Swapchain | FIFO, identity transform; no rotation handling. |
| Vulkan version | Requires a 1.1 device (older phones are skipped with a log line). |

## Not part of the backend, still to write

- Android CMake project: SDL3 activity, glslang (`glslang`, `SPIRV`, `glslang-default-resource-limits`), the generated
  code linked in (no `X86_GAME_CODE`), `gpu/d3d12.cpp` and `audio/xaudio2.cpp` left out.
- Audio backend (Oboe / AAudio / SDL audio) in place of `audio/xaudio2.cpp`.
- App shell for `app/*` and `core/input.cpp`: touch and gamepad input, picking the game folder.

# Android OpenGL ES 3.2 backend: status

`sdk/runtime/gpu/gles.cpp` is a second `gpu::Backend` for phones whose Vulkan driver crashes or draws wrongly. It
draws the same way as the Vulkan backend: the API-neutral parts (draw constants, shader keys, texture signatures, DXT
decoding, frame pacing) are shared in `gpu/host_common.cpp`, and the shaders are the same HLSL, compiled to SPIR-V by
`gpu/spirv.cpp` and written out as GLSL ES 3.20 by SPIRV-Cross (`gpu/gles_shader.cpp`). Which backend starts is decided
in `gpu/android_display.cpp` (Settings > Graphics > Renderer: Auto, Vulkan, OpenGL ES; see `android/README.md`).

Conventions:

| What | How |
|---|---|
| Rows | Every vertex shader flips clip y, so targets keep their rows top first as on the console and Vulkan; scissors and copies use the console's rows, `Present` flips while blitting to the window. The front face is swapped (GL sees the winding mirrored). |
| Depth | Reversed as on Vulkan, `GL_DEPTH32F_STENCIL8`. With `GL_EXT_clip_control` the shaders keep D3D's 0..1 clip z; without it SPIRV-Cross maps it to -1..1. |
| Resources | Uniform blocks `VC` (binding 0) and `DC` (binding 1); `tN`/`sN` become one combined sampler at unit N with a GL sampler object. |
| Textures | Guest BGRA uploaded as RGBA with a red/blue texture swizzle; BC native with `GL_EXT_texture_compression_s3tc`, else decoded on the CPU; R16 as `GL_R16_EXT` or floats; float formats point-sampled without `GL_OES_texture_float_linear`. |
| Streaming | Vertices, indices and constants in per-frame ring regions (3 frames), persistently mapped with `GL_EXT_buffer_storage`, else mapped unsynchronized per write; EGL fences per frame, which `WaitGpuFrame` waits on from any thread. |
| Threads | The context is made in `Init` on the title's thread and taken by the render thread on its first call. |

What is checked so far:

- `tools-build` target `shader_probe` translates the probe shaders (fixed function, quad, shadow, cube) to GLSL ES 3.20,
  with and without clip control, and glslang parses every one as an ES 3.20 shader and links vertex/pixel pairs.
- The APK builds (`android/build-apk.sh`) with both backends linked.

Nothing has run on a device yet.

## Not done yet (OpenGL ES)

| Area | State |
|---|---|
| Everything listed for Vulkan above | Same gaps: read-back, CPU writes to targets, feedback reads, occlusion queries, gamma, render-scale extensions. |
| Point lists | Not drawn (as on Vulkan). `PointHlsl` needs `surf.w`, which neither Android backend fills yet. |
| Mip LOD bias | ES samplers have no LOD bias; ignored. |
| Wireframe | ES has no polygon mode; drawn solid. |
| `noperspective` | Kept only with `GL_NV_shader_noperspective_interpolation`; else the screen quad's extra input is perspective (the same across a quad of one w). |
| Program binaries | Not cached between runs: shaders compile again on each start. |
| Vulkan blocklist | `kVulkanBlocklist` is empty: add GPUs as players report them. |
