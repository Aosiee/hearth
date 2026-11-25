#pragma once

// What Hearth (and a library built on it) needs from the program it runs in,
// resolved at link time: the program defines these free functions. An
// application with no logging or profiler of its own links
// hearth_default_host; an engine defines its own and routes them into its
// log, its profiler and its crash reporter.
//
// Free functions rather than a table of hooks, because a table someone
// fills in at startup would be process-global mutable state. Every function
// may be called from any thread, and before or after the rest of the host
// has started.

#include <cstdint>

namespace hearth::host {

	enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

	// One formatted line in a category (hearth/Log.h).
	void Log(LogLevel level, const char* category, const char* message);

	// A failed HEARTH_ASSERT (hearth/Assert.h). `message` is the assert's
	// text. Returns true to trap into the debugger, false to continue.
	bool AssertFailed(const char* expression, const char* message, const char* file, int line);

	// A profiler zone (hearth/Zone.h): Begin returns a handle End is given
	// back. A host without a profiler returns 0 and ignores End.
	uint64_t BeginZone(const char* name, const char* file, int line);
	void EndZone(uint64_t handle);

	// The GPU the program runs on, once known: a crash report names it.
	void NoteGpu(const char* name, const char* driver);

} // namespace hearth::host
