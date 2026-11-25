// The same macros in a translation unit built without asserts or zones:
// nothing reaches the host, and an assert's condition still runs once.

#undef HEARTH_ENABLE_ASSERTS
#undef HEARTH_ENABLE_ZONES
#include "hearth/Assert.h"
#include "hearth/Zone.h"

#include "TestHost.h"
#include "hamr/test.h"

TEST_CASE("disabled: an assert evaluates its condition and calls nothing")
{
	hearth::test::Reset();
	int evaluated = 0;
	HEARTH_ASSERT(++evaluated == 2, "would fail");
	CHECK(evaluated == 1);
	CHECK(hearth::test::Calls().empty());
}

TEST_CASE("disabled: a zone is nothing")
{
	hearth::test::Reset();
	{
		HEARTH_ZONE("off");
		HEARTH_ZONE_FUNCTION();
	}
	CHECK(hearth::test::Calls().empty());
}
