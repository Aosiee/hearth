// The descriptor log's recycle step drops only the recycled binding's
// entries - the texture and buffer arrays share index numbers, and the old
// texture recycle erased a pending buffer write at the same number (a
// RenderScene's frame data stayed unwritten).

#include "hearth/rhi/vulkan/DescriptorLogUtil.h"

#include "hamr/test.h"

#include <cstdint>
#include <vector>

namespace DescriptorLogUtilTestsDetail {

	struct Entry
	{
		uint64_t Seq = 0;
		uint32_t Index = 0;
		uint32_t Binding = 0;
	};

}

TEST_CASE("rhi descriptor log: a recycle drops only its own binding's entries, in seq order")
{
	using DescriptorLogUtilTestsDetail::Entry;
	std::vector<Entry> log = {
		{ 1, 64, 0 }, // the texture tombstone the recycle supersedes
		{ 2, 64, 1 }, // a RenderScene's pending frame-data write - must stay
		{ 3, 65, 0 },
		{ 4, 64, 0 },
		{ 5, 64, 1 },
	};
	hearth::rhi::DropSupersededEntries(log, 64, 0);
	REQUIRE(log.size() == 3);
	CHECK(log[0].Seq == 2);
	CHECK(log[1].Seq == 3);
	CHECK(log[2].Seq == 5);
	for (const Entry& entry : log)
		CHECK(!(entry.Index == 64 && entry.Binding == 0));

	hearth::rhi::DropSupersededEntries(log, 64, 1);
	REQUIRE(log.size() == 1);
	CHECK(log[0].Index == 65);
	CHECK(log[0].Binding == 0);
}
