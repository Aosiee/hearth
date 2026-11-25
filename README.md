# Hearth

[![Linux](https://github.com/Aosiee/hearth/actions/workflows/linux.yml/badge.svg?branch=dev)](https://github.com/Aosiee/hearth/actions/workflows/linux.yml)
[![Windows: not supported](https://img.shields.io/badge/Windows-not%20supported-lightgrey)](#building)
[![macOS: not supported](https://img.shields.io/badge/macOS-not%20supported-lightgrey)](#building)

[![Release](https://img.shields.io/github/v/release/Aosiee/hearth)](https://github.com/Aosiee/hearth/releases/latest)

The base a real-time application stands on: the OS platform (windows,
events, input), the frame loop and the GPU layer, shared by an engine and
the tools around it. A game engine is one Hearth program; a small tool
window built with a UI library is another, with nothing else linked.

Hearth is being built in stages. Today it is the **host seam** (how Hearth
and the libraries on it log, assert, profile and report a GPU without
depending on the program's own logging, profiler or crash reporter) and the
**platform** (windows, events, input, the clipboard, gamepads, Vulkan
surfaces) and the **GPU layer** (a Vulkan device behind a small RHI, and a
render graph). The frame loop follows.

Hearth was written for [TinyEngine](https://github.com/Aosiee/TinyEngine)
and knows nothing about it.

## The host seam

A program defines a handful of free functions (`include/hearth/Host.h`),
resolved at link time. An engine routes them into its own log and
profiler; a program without either links `hearth::default_host` (lines on
stderr, a trap on a failed assert).

```cpp
#include "hearth/Assert.h"
#include "hearth/Log.h"
#include "hearth/Zone.h"

HEARTH_LOG_CATEGORY(LogVulkan)

void CreateDevice()
{
    HEARTH_ZONE_FUNCTION();
    HEARTH_LOG_INFO(LogVulkan, "{} queue families", familyCount);
    HEARTH_ASSERT(familyCount > 0, "no queue families");
}
```

- **Asserts** are checked where `HEARTH_ENABLE_ASSERTS` is defined (Debug
  in Hearth's own build); elsewhere the condition still runs and is
  discarded.
- **Zones** compile in where `HEARTH_ENABLE_ZONES` is defined (everywhere
  unless `-DHEARTH_NO_ZONES=ON`).
- **Categories** are read-only names, so declaring one creates no global
  anything can write.

## The platform

`hearth::platform` is the window system: one `Platform` per process, and
the `Window`s made from it. GLFW is its implementation and stays inside its
sources: nothing else, in Hearth or in a program on it, includes GLFW.

```cpp
#include "hearth/platform/Platform.h"
#include "hearth/platform/Window.h"

auto platform = hearth::platform::Platform::Create();
hearth::platform::Window window(*platform, { .Title = "Hello", .Width = 640, .Height = 400 });

for (bool running = true; running;)
{
    platform->Pump();
    for (const hearth::platform::Event& e : platform->TakeEvents())
        if (e.Type == hearth::platform::EventType::WindowClose)
            running = false;
}
```

- **Events are values**, each naming the window it came from, in the order
  the window system sent them.
- **The backend** is the platform's choice, or the one asked for
  (`Backend::X11`, `Wayland`, `Null` for headless runs and tests), with a
  fallback when the machine cannot provide it.
- **Capabilities** say what the window system allows: Wayland, for one,
  lets no program place its windows or read their positions.

## The GPU layer

`hearth::rhi` is a Vulkan 1.3 device behind a small interface
(`hearth/rhi/Device.h`, `CommandList.h`): buffers, images, samplers,
shaders, graphics and compute pipelines, command lists, submits on one
graphics timeline. `hearth::rg` is a render graph over it: passes declare
what they read and write, and the graph orders them, culls the unused ones
and derives every barrier.

```cpp
#include "hearth/rhi/RenderGraph.h"

rg::RenderGraph graph(pool);
rg::RGBuffer target = graph.ImportBuffer(buffer, rhi::BufferState::Undefined);
auto& fill = graph.AddPass("Fill");
fill.Write(target, rhi::BufferState::ComputeWrite);
fill.SetExecute([&](rhi::CommandList& list, const rg::PassResources&) {
    list.BindPipeline(pipeline);
    list.BindGlobalSet();
    list.Dispatch(groups, 1, 1);
});
graph.Compile();
graph.Execute(device, list);
```

- **One bindless set per frame slot.** Every texture and storage buffer
  has an index in it; a write reaches a slot's set when that slot next
  starts recording, so writing a live index is always safe.
- **Nothing is destroyed while a frame may use it.** A destroyed object
  goes to the retire queue and is freed once its frame slot has been
  reset; a bindless index is reused only after every slot has seen it
  cleared.
- **The program's windows** implement `VulkanWindowContext` (main or not,
  the slot being built); the program's per-window frame records and
  presents.
- **Developer tools** (validation layers, object names, debug labels, GPU
  timestamps for the program's profiler and frame debugger) build with
  `HEARTH_GPU_DEBUG` (`-DHEARTH_GPU_DEBUG=OFF` leaves them out).

## Building

C++20 (`std::format`), GLFW (TinyEngine's fork, fetched at its pinned
commit) and Vulkan-Headers for the platform; volk, the Vulkan Memory
Allocator, loom, BLAKE3 and SPIRV-Cross for the GPU layer; and for the
suite hamr's C++ test library. Linux is what is checked today;
`-DHEARTH_NULL_PLATFORM_ONLY=ON` builds GLFW with its null platform alone
(no window-system libraries, as CI does).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/hearth_tests && build/hearth_platform_tests && build/hearth_default_host_tests
build/hearth_rhi_tests && build/hearth_rhi_gpu_tests   # the second needs a Vulkan device
```

Each dependency is fetched at the commit or tag `CMakeLists.txt` pins;
`-DHEARTH_<NAME>_DIR=<dir>` uses a local checkout. A host build adds Hearth
with `add_subdirectory` and links `hearth::core`, `hearth::platform` or
`hearth::rhi` (plus `hearth::default_host` if it has no host of its own).
The GPU suite runs on any Vulkan 1.3 device; CI uses Mesa's lavapipe. The full suite, side by side and in every configuration, runs
through [hamr](https://github.com/Aosiee/hamr): `hamr run` (`--tier
nightly` adds the sanitizers).

## Branches and releases

`main` holds releases only, each one tagged. New work is built on a stream
branched from `dev`, lands on `dev` once it passes locally, and is checked
there by CI; when enough has gathered, `dev` becomes the next release. See
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

The Hearth License: MIT-style, with one condition: a product that includes
Hearth credits it in its credits or about box. See [LICENSE](LICENSE).
