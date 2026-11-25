# Hearth

[![Linux](https://github.com/Aosiee/hearth/actions/workflows/linux.yml/badge.svg)](https://github.com/Aosiee/hearth/actions/workflows/linux.yml)
[![Windows: not yet](https://img.shields.io/badge/Windows-not%20yet-lightgrey)](#building)
[![macOS: not yet](https://img.shields.io/badge/macOS-not%20yet-lightgrey)](#building)

The base a real-time application stands on: the OS platform (windows,
events, input), the frame loop and the GPU layer, shared by an engine and
the tools around it. A game engine is one Hearth program; a small tool
window built with a UI library is another, with nothing else linked.

Hearth is being built in stages. Today it is the **host seam** (how Hearth
and the libraries on it log, assert, profile and report a GPU without
depending on the program's own logging, profiler or crash reporter) and the
**platform** (windows, events, input, the clipboard, gamepads, Vulkan
surfaces). The frame loop and the GPU layer follow.

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

## Building

C++20 (`std::format`), GLFW (TinyEngine's fork, fetched at its pinned
commit) and Vulkan-Headers for the platform, and for the suite hamr's C++
test library. Linux is what is checked today;
`-DHEARTH_NULL_PLATFORM_ONLY=ON` builds GLFW with its null platform alone
(no window-system libraries, as CI does).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/hearth_tests && build/hearth_platform_tests && build/hearth_default_host_tests
```

hamr is fetched at the commit `CMakeLists.txt` pins; `-DHEARTH_HAMR_DIR=<dir>`
uses a local checkout. A host build adds Hearth with `add_subdirectory` and
links `hearth::core` (plus `hearth::default_host` if it has no host of its
own). The full suite, side by side and in every configuration, runs
through [hamr](https://github.com/Aosiee/hamr): `hamr run` (`--tier
nightly` adds the sanitizers).

## License

The Hearth License: MIT-style, with one condition: a product that includes
Hearth credits it in its credits or about box. See [LICENSE](LICENSE).
