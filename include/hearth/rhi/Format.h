#pragma once

// Exactly the formats in use today, mapped 1:1 to the backend's pixel formats in the
// implementation. Extended on demand, never speculatively.

#include <cstdint>

namespace hearth::rhi {

	enum class Format : uint32_t
	{
		Undefined,

		B8G8R8A8_UNORM,
		B8G8R8A8_SRGB,
		R8G8B8A8_UNORM,
		R8G8B8A8_SRGB,
		R8_UNORM,
		D32_SFLOAT,

		// The G-buffer's GB1 octahedral normal,
		// the HDR SceneColor intermediate, and the velocity target.
		R16G16_UNORM,
		R16G16B16A16_SFLOAT,
		R16G16_SFLOAT,

		// The vertex attribute formats (extended on demand).
		R32G32B32_SFLOAT,
		R32G32B32A32_SFLOAT,
		R32G32_SFLOAT,
		R32_SFLOAT,
	};

} // namespace hearth::rhi
