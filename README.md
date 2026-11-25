# Hearth

[![Linux](https://github.com/Aosiee/hearth/actions/workflows/linux.yml/badge.svg)](https://github.com/Aosiee/hearth/actions/workflows/linux.yml)
[![Windows: not yet](https://img.shields.io/badge/Windows-not%20yet-lightgrey)](#building)
[![macOS: not yet](https://img.shields.io/badge/macOS-not%20yet-lightgrey)](#building)

The base a real-time application stands on: the OS platform (windows,
events, input), the frame loop and the GPU layer, shared by an engine and
the tools around it. A game engine is one Hearth program; a small tool
window built with a UI library is another, with nothing else linked.

Hearth is being built in stages. Today it is the **host seam**: how Hearth
and the libraries on it log, assert, profile and report a GPU without
depending on the program's own logging, profiler or crash reporter. The
platform, the loop and the GPU layer follow.

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

## Building

C++20 (`std::format`), and for the suite hamr's C++ test library. Linux is
what is checked today.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/hearth_tests && build/hearth_default_host_tests
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
