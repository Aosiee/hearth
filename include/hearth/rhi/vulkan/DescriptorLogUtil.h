#pragma once

// The descriptor write log's pure helpers (templated on the entry so
// the suite drives them without a device).

#include <cstdint>
#include <vector>

namespace hearth::rhi {

	// A recycled bindless index's earlier entries in ONE binding are
	// superseded by its new owner's write - dropped so the log stays bounded
	// per index. The other binding shares the index numbers and must keep
	// its entries (a texture recycle that dropped them would lose a pending
	// buffer write at the same number). The log's seq order is preserved.
	template <typename Entry>
	void DropSupersededEntries(std::vector<Entry>& log, uint32_t index, uint32_t binding)
	{
		std::erase_if(log, [index, binding](const Entry& entry) { return entry.Index == index && entry.Binding == binding; });
	}

} // namespace hearth::rhi
