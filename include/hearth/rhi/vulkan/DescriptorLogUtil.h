#pragma once

// The descriptor write log's pure helpers (templated on the entry so
// tinytest drives them without a device).

#include <cstdint>
#include <vector>

namespace TinyEngine { namespace rhi {

	// A recycled bindless index's earlier entries in ONE binding are
	// superseded by its new owner's write - dropped so the log stays bounded
	// per index. The other binding shares the index numbers and must keep
	// its entries (the texture recycle dropped a RenderScene's pending
	// frame-data buffer writes - the scene drew black). The log's seq order
	// is preserved.
	template <typename Entry>
	void DropSupersededEntries(std::vector<Entry>& log, uint32_t index, uint32_t binding)
	{
		std::erase_if(log, [index, binding](const Entry& entry) { return entry.Index == index && entry.Binding == binding; });
	}

}} // namespace TinyEngine::rhi
