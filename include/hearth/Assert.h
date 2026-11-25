#pragma once

// HEARTH_ASSERT(condition, "what went wrong"): checked in a build that
// defines HEARTH_ENABLE_ASSERTS (Hearth's CMake build does in Debug; a host
// build chooses), where a failure goes to the host (hearth/Host.h), which
// logs it, notes it for a crash report and asks for a trap. Without the
// define the condition is still evaluated and discarded, so a variable used
// only by an assert stays used and the condition's side effects are the
// same in every build.

#include "hearth/Host.h"

#if defined(_MSC_VER)
	#define HEARTH_DEBUGBREAK() __debugbreak()
#else
	#include <csignal>
	#define HEARTH_DEBUGBREAK() std::raise(SIGTRAP)
#endif

#if defined(HEARTH_ENABLE_ASSERTS)
	#define HEARTH_ASSERT(condition, message) \
		do { \
			if (!(condition) && ::hearth::host::AssertFailed(#condition, message, __FILE__, __LINE__)) \
				HEARTH_DEBUGBREAK(); \
		} while (0)
#else
	#define HEARTH_ASSERT(condition, message) ((void)(condition))
#endif
