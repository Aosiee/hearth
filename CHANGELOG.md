# Changelog

Each release is an annotated tag (`v0.x.0`, or `v0.x.y` for a release of
fixes only) whose message is the entry below. Versions before 1.0 may change
the API; every entry says what callers need to change.

## Unreleased

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
