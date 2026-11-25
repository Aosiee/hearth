// The host for a program with no logging, profiler or crash reporter of its
// own: lines go to stderr, a failed assert traps, zones and the GPU note do
// nothing.

#include "hearth/Host.h"

#include <cstdio>

namespace hearth::host {

	namespace {
		const char* LevelName(LogLevel level)
		{
			switch (level)
			{
			case LogLevel::Trace: return "trace";
			case LogLevel::Debug: return "debug";
			case LogLevel::Info: return "info";
			case LogLevel::Warn: return "warn";
			case LogLevel::Error: return "error";
			}
			return "?";
		}
	}

	void Log(LogLevel level, const char* category, const char* message)
	{
		// One fprintf per line, so lines from two threads do not interleave.
		std::fprintf(stderr, "[%s] %s: %s\n", LevelName(level), category, message);
	}

	bool AssertFailed(const char* expression, const char* message, const char* file, int line)
	{
		std::fprintf(stderr, "[error] Hearth: assertion failed: %s (%s) at %s:%d\n", message, expression, file, line);
		return true;
	}

	uint64_t BeginZone(const char*, const char*, int) { return 0; }
	void EndZone(uint64_t) {}
	void NoteGpu(const char*, const char*) {}

} // namespace hearth::host
