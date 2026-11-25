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
