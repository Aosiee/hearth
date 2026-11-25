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
surfaces), the **frame loop** (fixed phases the program's steps register
into), the **GPU layer** (a Vulkan device behind a small RHI, a render
graph, and the presenter that runs a window's frame) and the **app** (all of
them put together for a program that is not an engine).

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

## The frame loop

`hearth::Loop` (`include/hearth/Loop.h`, in `hearth::core`) runs frames in
fixed phases: BeginFrame, Pump, Events, Input, Update, Fixed, Simulate,
Present, EndFrame. A program registers each of its per-frame steps as a
client of a phase, ordered by a key; the loop owns the clock, the frame's
number and delta, the fixed-step accumulator and the frame cap.

```cpp
#include "hearth/Loop.h"

hearth::Loop loop;
loop.SetFixedStep(1.0 / 60.0, 8);   // at most 8 steps a frame; the rest is dropped
loop.SetFrameCap(1.0 / 144.0);
loop.Add(hearth::Phase::Pump, 100, "pump", [&](const hearth::Frame&) { platform.Pump(); }, { .RunsPaused = true });
loop.Add(hearth::Phase::Update, 100, "game.update", [&](const hearth::Frame& frame) { game.Update(frame.Delta); });
loop.AddFixed(100, "game.step", [&](const hearth::Frame&, const hearth::FixedStep& step) { game.Step(step.Step); });
loop.Add(hearth::Phase::Present, 100, "present", [&](const hearth::Frame&) { presenter.Present(); }, { .RunsPaused = true });
loop.Run();                         // until loop.Stop()
```

- **The order** is logged when the loop starts (`LogLoop`); a key another
  client of the phase holds, or a name another client holds, is refused.
  `Order()` lists it, so a program can check it against what it expects.
- **Fixed** runs its clients once per fixed step: none, one or several a
  frame, each seeing the step's tick and the time still to step.
- **Paused frames** (`RunPaused(keepPaused)`, called from a client) run
  every phase but Fixed and Simulate, with only the clients registered
  with `RunsPaused`: a script debugger keeps the window responsive this
  way while the simulation stands still.
- **Zones:** each client runs in a profiler zone named by it
  (`.Zone = false` opts out).
- `SetClock`, `SetSleep` and `SetFixedDelta` make a run deterministic (a
  capture) or testable.

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
- **A window's frame** is a `WindowPresenter` (`hearth/rhi/vulkan/WindowPresenter.h`):
  its swapchain (none offscreen), its frame slots, and a submit thread that
  acquires, records, submits and presents while the loop thread builds the
  next frame. What a frame draws comes from its `FrameClient`s, called in
  order at fixed points: an update on the loop thread, the publish into
  the frame's slot, then on the submit thread the client's own graphs and
  its passes into the swapchain graph. A client that needs every window's
  GPU work idle (a shader reload) asks for the park first.

```cpp
class Scene final : public rhi::FrameClient
{
    void Update(const rhi::FrameUpdate& update) override { /* build the frame */ }
    void RecordSwapchain(rhi::SwapchainGraph& swapchain, const rhi::FrameRecord&) override
    {
        auto& pass = swapchain.Graph().AddPass("Scene");
        pass.Write(swapchain.Image(), rhi::TextureState::RenderTarget);
        pass.SetExecute([&swapchain](rhi::CommandList& list, const rg::PassResources&) {
            auto& vk = static_cast<rhi::VulkanCommandList&>(list);
            swapchain.BeginRendering(vk, false);
            // draw
            vkCmdEndRendering(vk.GetCommandBuffer());
        });
    }
};

rhi::WindowPresenter presenter(device, { .Window = window, .IsMainWindow = true });
rhi::FrameClient* clients[] = { &scene, &ui };
presenter.SetClients(clients);
while (running)
    presenter.Present();
```
- **Offscreen**, a presenter has no swapchain. Given `OffscreenImage`, it
  draws its frames into an image of its own (colour and depth) through the
  same swapchain graph, so its clients draw as they do into a window, and
  `ReadOffscreenImage` reads the pixels back.
- **Developer tools** (validation layers, object names, debug labels, GPU
  timestamps for the program's profiler and frame debugger) build with
  `HEARTH_GPU_DEBUG` (`-DHEARTH_GPU_DEBUG=OFF` leaves them out).

## The app

`hearth::App` (`hearth/App.h`, `hearth::app`) is all of the above for a
program that is not an engine: the platform, the main window, a thread
pool, the device, the main window's presenter and the frame loop, plus the
command-line flags such programs share.

```cpp
#include "hearth/App.h"

int main(int argc, char** argv)
{
    hearth::App app(hearth::App::ParseArgs(argc, argv, { .Title = "Hello", .Width = 640, .Height = 400 }));
    app.AddFrameClient(scene);                                  // what the window draws
    app.AddEventHandler([&](const hearth::platform::Event& e) { return scene.OnEvent(e); });
    app.GetLoop().Add(hearth::Phase::Update, 100, "scene.update", [&](const hearth::Frame& f) { scene.Update(f.Delta); });
    return app.Run();
}
```

- **The flags:** `--display-mode windowed|hidden|offscreen`,
  `--render-target WxH`, `--capture-ui <png>`, `--fixed-dt <seconds>`,
  `--frames N`, `--min-frames N`, `--platform <backend>`. The rest stay in
  `AppDesc::Args` for the program.
- **Offscreen** there is no window and no display server: the platform is
  the null one and the frames draw into the presenter's offscreen image.
- **The capture:** `--capture-ui` (offscreen) writes the PNG at the first
  frame past `--min-frames` (and the program's `SetCaptureReady`), with a
  sidecar `<png>.json` holding the BLAKE3 of its pixels; the run exits 0.
  A frame limit reached first exits 3, a flag it cannot read 2.

## Building

C++20 (`std::format`), GLFW (TinyEngine's fork, fetched at its pinned
commit) and Vulkan-Headers for the platform; volk, the Vulkan Memory
Allocator, loom, BLAKE3 and SPIRV-Cross for the GPU layer; stb_image_write
for the app's capture; and for the suite hamr's C++ test library. Linux is what is checked today;
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
with `add_subdirectory` and links `hearth::core`, `hearth::platform`,
`hearth::rhi` or `hearth::app` (plus `hearth::default_host` if it has no
host of its own).
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
