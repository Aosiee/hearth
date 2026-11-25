// The host seam as the library uses it: the log, assert and zone macros
// reach the host with the level, category, text and source position.

#ifndef HEARTH_ENABLE_ASSERTS
#define HEARTH_ENABLE_ASSERTS
#endif
#ifndef HEARTH_ENABLE_ZONES
#define HEARTH_ENABLE_ZONES
#endif
#include "hearth/Assert.h"
#include "hearth/Log.h"
#include "hearth/Zone.h"

#include "TestHost.h"
#include "hamr/test.h"

#include <cstring>
#include <string>

using hearth::test::Calls;

HEARTH_LOG_CATEGORY(LogHearthTest)

TEST_CASE("log: a line reaches the host formatted, with its level and category")
{
	hearth::test::Reset();
	HEARTH_LOG_INFO(LogHearthTest, "{} of {}", 3, "four");
	HEARTH_LOG_ERROR(LogHearthTest, "positional {0} {1} {0}", 'a', 2.5);
	REQUIRE(Calls().size() == 2);
	CHECK(Calls()[0].What == "log");
	CHECK(Calls()[0].Level == static_cast<int>(hearth::host::LogLevel::Info));
	CHECK(Calls()[0].Category == "LogHearthTest");
	CHECK(Calls()[0].Text == "3 of four");
	CHECK(Calls()[1].Level == static_cast<int>(hearth::host::LogLevel::Error));
	CHECK(Calls()[1].Text == "positional a 2.5 a");
}

TEST_CASE("log: every level has its macro")
{
	hearth::test::Reset();
	HEARTH_LOG_TRACE(LogHearthTest, "t");
	HEARTH_LOG_DEBUG(LogHearthTest, "d");
	HEARTH_LOG_INFO(LogHearthTest, "i");
	HEARTH_LOG_WARN(LogHearthTest, "w");
	HEARTH_LOG_ERROR(LogHearthTest, "e");
	REQUIRE(Calls().size() == 5);
	for (int i = 0; i < 5; ++i)
		CHECK(Calls()[i].Level == i);
}

TEST_CASE("log: a category is one read-only object, the same from every call")
{
	CHECK(&LogHearthTest() == &LogHearthTest());
	CHECK(std::strcmp(LogHearthTest().Name, "LogHearthTest") == 0);
}

TEST_CASE("assert: a failed assert reaches the host with its expression and position")
{
	hearth::test::Reset();
	const int value = 3;
	HEARTH_ASSERT(value == 4, "value must be four");
	const int line = __LINE__ - 1;
	REQUIRE(Calls().size() == 1);
	CHECK(Calls()[0].What == "assert");
	CHECK(Calls()[0].Text == "value must be four");
	CHECK(Calls()[0].Detail == "value == 4");
	CHECK(Calls()[0].Line == line);
	CHECK(Calls()[0].File.find("HostTests.cpp") != std::string::npos);
}

TEST_CASE("assert: a passing assert calls nothing, and its condition runs once")
{
	hearth::test::Reset();
	int evaluated = 0;
	HEARTH_ASSERT(++evaluated == 1, "runs once");
	CHECK(evaluated == 1);
	CHECK(Calls().empty());
}

TEST_CASE("assert: it is one statement, so it fits an unbraced if")
{
	hearth::test::Reset();
	const bool check = true;
	if (check)
		HEARTH_ASSERT(false, "taken");
	else
		HEARTH_ASSERT(false, "not taken");
	REQUIRE(Calls().size() == 1);
	CHECK(Calls()[0].Text == "taken");
}

TEST_CASE("zone: a scope opens a zone and closes it with the host's handle")
{
	hearth::test::Reset();
	{
		HEARTH_ZONE("outer");
		{
			HEARTH_ZONE_FUNCTION();
		}
	}
	REQUIRE(Calls().size() == 4);
	CHECK(Calls()[0].What == "begin");
	CHECK(Calls()[0].Text == "outer");
	CHECK(Calls()[1].What == "begin");
	CHECK(!Calls()[1].Text.empty()); // the enclosing function's name
	CHECK(Calls()[2].What == "end");
	CHECK(Calls()[2].Handle == Calls()[1].Handle);
	CHECK(Calls()[3].What == "end");
	CHECK(Calls()[3].Handle == Calls()[0].Handle);
}

TEST_CASE("gpu: the note passes the name and driver through")
{
	hearth::test::Reset();
	hearth::host::NoteGpu("Test GPU", "1.2.3");
	REQUIRE(Calls().size() == 1);
	CHECK(Calls()[0].Text == "Test GPU");
	CHECK(Calls()[0].Detail == "1.2.3");
}
