#pragma once

// The suite's host: it records every call, so a case can check what reached
// the host.

#include "hearth/Host.h"

#include <string>
#include <vector>

namespace hearth::test {

	struct Call
	{
		std::string What;        // "log", "assert", "begin", "end", "gpu"
		int Level = -1;
		std::string Category, Text, Detail, File;
		int Line = 0;
		uint64_t Handle = 0;
	};

	// The calls since the last Reset, in order. One case at a time uses it:
	// the cases that touch the host do not run threads.
	std::vector<Call>& Calls();
	void Reset();

} // namespace hearth::test
