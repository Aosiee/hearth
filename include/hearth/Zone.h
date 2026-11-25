#pragma once

// Profiler zones through the host (hearth/Host.h): HEARTH_ZONE("name") or
// HEARTH_ZONE_FUNCTION() opens a zone that closes at the end of the scope.
// Compiled in when HEARTH_ENABLE_ZONES is defined (Hearth's CMake build
// does unless HEARTH_NO_ZONES; a host build chooses); otherwise they are
// nothing.

#include "hearth/Host.h"

namespace hearth {

	class Zone
	{
	public:
		Zone(const char* name, const char* file, int line) : m_Handle(host::BeginZone(name, file, line)) {}
		~Zone() { host::EndZone(m_Handle); }
		Zone(const Zone&) = delete;
		Zone& operator=(const Zone&) = delete;

	private:
		uint64_t m_Handle;
	};

} // namespace hearth

#define HEARTH_ZONE_CONCAT_(a, b) a##b
#define HEARTH_ZONE_NAME_(line) HEARTH_ZONE_CONCAT_(hearthZone, line)

#if defined(HEARTH_ENABLE_ZONES)
	#if defined(_MSC_VER)
		#define HEARTH_FUNCTION_NAME_ __FUNCSIG__
	#else
		#define HEARTH_FUNCTION_NAME_ __PRETTY_FUNCTION__
	#endif
	#define HEARTH_ZONE(name) ::hearth::Zone HEARTH_ZONE_NAME_(__LINE__)(name, __FILE__, __LINE__)
	#define HEARTH_ZONE_FUNCTION() HEARTH_ZONE(HEARTH_FUNCTION_NAME_)
#else
	#define HEARTH_ZONE(name) ((void)0)
	#define HEARTH_ZONE_FUNCTION() ((void)0)
#endif
