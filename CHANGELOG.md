# Changelog

Each release is an annotated tag (`v0.x.0`, or `v0.x.y` for a release of
fixes only) whose message is the entry below. Versions before 1.0 may change
the API; every entry says what callers need to change.

## 0.3.0

A window's frame: TinyEngine's window presenter, as
`hearth::rhi::WindowPresenter`.

- `WindowPresenter`: one window's frame on the GPU. It owns the window's
  swapchain (none for an offscreen window), its frame slots on the device,
  the submit thread that acquires, records, submits and presents, and the
  parks that wait for every window.
- `FrameClient`: what a frame draws, supplied by the program as clients the
  presenter calls in order at fixed points: the park, the update, the
  publish, the target and swapchain records, and the end of the frame.
  `SwapchainGraph` is the swapchain's render graph, which the presenter
  opens with the clear and closes with the present transition.
- `VulkanDevice::TryGetGpuFrameDebugSink`: the frame-debug sink is
  optional, and the presenter writes frame data only when one is set.
- `hearth::default_host` is an object library, so it links whatever its
  place on the link line (as an archive, GNU ld dropped it when it came
  before `hearth::platform` or `hearth::rhi`).
- `hearth_rhi_gpu_tests` gains the presenter's cases: the clients' order
  and slots, the park, and a client's graph in the frame's submit.

Nothing changes for callers of 0.2: the presenter is new.

## 0.2.0

The GPU layer: TinyEngine's RHI, render graph and Vulkan device, as
`hearth::rhi`.

- `hearth::rhi`: the GPU layer, moved here from TinyEngine with its
  history. The RHI interface (`Device`, `CommandList`, states, formats,
  barriers), the render graph (`hearth::rg`), and their Vulkan 1.3
  implementation: the device and its graphics timeline, swapchains,
  pipelines and the pipeline cache (with its on-disk file), shader modules
  and reflection, buffers, `VulkanImage2D`, the per-slot bindless set and
  its write log, the retire queue.
- `VulkanWindowContext`: what the device needs from the program's windows.
- `GpuTimingSink` and the optional `GpuFrameDebugSink`: GPU timing and
  frame data for the program's profiler and frame debugger.
- `HEARTH_GPU_DEBUG`: the developer tools, on by default.
- New dependencies, fetched at pins: volk, the Vulkan Memory Allocator,
  loom, BLAKE3, SPIRV-Cross.
- The suite gains `hearth_rhi_tests` and `hearth_rhi_gpu_tests` (run on
  Mesa's lavapipe in CI).

Nothing changes for callers of 0.1: the new module is added beside
`hearth::core` and `hearth::platform`.

## 0.1.1

- GLFW is fetched at the fork's reworded commit (`21b45cdf`, the same
  tree): the fork's history was rewritten, and 0.1.0's commit can no
  longer be fetched, so 0.1.0 does not build from source.

Nothing changes for callers.

## 0.1.0

The first release: the base TinyEngine and Wisp stand on, as a library of
its own.

- The host seam (`hearth/Host.h`): logging, failed asserts, profiler zones
  and the GPU note, defined by the program at link time.
- `hearth/Log.h` (read-only categories, std::format lines),
  `hearth/Assert.h` and `hearth/Zone.h` on top of it.
- `hearth::default_host` for a program without a host of its own.
- `hearth::platform`: the window system on GLFW (TinyEngine's fork), the
  only Hearth code that includes it. `Platform` (one per process: the
  backend choice and its fallback, the event pump and queue, the clock,
  gamepads, the clipboard, the capabilities, Vulkan's instance extensions
  and present support) and `Window` (size, framebuffer size, content
  scale, position where the window system allows it, title, focus, key
  and button state, the cursor's position, mode and shape, a Vulkan
  surface, the program's own pointer). Events are values, each naming its
  window, taken in order after a pump. `hearth/platform/Keys.h` holds the
  key, mouse, modifier and gamepad codes (GLFW's values).
