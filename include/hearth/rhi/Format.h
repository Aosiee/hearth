#pragma once

// Exactly the formats in use today, mapped 1:1 to VkFormat in the Vulkan
// implementation. Extended on demand, never speculatively.

#include <cstdint>

namespace TinyEngine {
namespace rhi {

	enum class Format : uint32_t
	{
		Undefined,

		B8G8R8A8_UNORM,
		B8G8R8A8_SRGB,
		R8G8B8A8_UNORM,
		R8_UNORM,
		D32_SFLOAT,
	};

} // namespace rhi
} // namespace TinyEngine
