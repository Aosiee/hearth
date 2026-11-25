// The default host, in its own binary (a program links one host): its lines
// on stderr, and a failed assert asking for the trap.

#define HAMR_TEST_MAIN
#include "hamr/test.h"

#include "hearth/Host.h"

#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

	// Runs `body` with stderr sent to a temporary file and returns what it wrote.
	template <typename Body>
	std::string CaptureStderr(Body body)
	{
		std::fflush(stderr);
		FILE* file = std::tmpfile();
		REQUIRE(file != nullptr);
		const int saved = dup(fileno(stderr));
		dup2(fileno(file), fileno(stderr));
		body();
		std::fflush(stderr);
		dup2(saved, fileno(stderr));
		close(saved);
		std::string text;
		std::rewind(file);
		char buffer[512];
		while (const size_t n = std::fread(buffer, 1, sizeof buffer, file))
			text.append(buffer, n);
		std::fclose(file);
		return text;
	}

} // namespace

TEST_CASE("default host: a line on stderr with its level and category")
{
	const std::string out = CaptureStderr([] { hearth::host::Log(hearth::host::LogLevel::Warn, "LogTest", "low disk"); });
	CHECK(out == "[warn] LogTest: low disk\n");
}

TEST_CASE("default host: a failed assert is reported and asks for the trap")
{
	bool trap = false;
	const std::string out = CaptureStderr([&] { trap = hearth::host::AssertFailed("x > 0", "x must be positive", "a.cpp", 12); });
	CHECK(trap);
	CHECK(out == "[error] Hearth: assertion failed: x must be positive (x > 0) at a.cpp:12\n");
}

TEST_CASE("default host: zones and the GPU note do nothing")
{
	const std::string out = CaptureStderr([] {
		hearth::host::EndZone(hearth::host::BeginZone("zone", "a.cpp", 1));
		hearth::host::NoteGpu("GPU", "driver");
	});
	CHECK(hearth::host::BeginZone("zone", "a.cpp", 1) == 0);
	CHECK(out.empty());
}
