#pragma once

// The API-neutral barrier description. The caller passes explicit
// from->to states (the renderers' hand-written layouts); the Vulkan
// implementation asserts the `from` state matches the tracked layout.
// Inside the RenderGraph the graph derives these and the caller passes
// nothing.

#include "hearth/rhi/Types.h"
#include "hearth/rhi/States.h"

#include <cstdint>

namespace hearth::rhi {

	struct TextureBarrier
	{
		TextureHandle Texture;
		TextureState  From = TextureState::Undefined;
		TextureState  To   = TextureState::Undefined;
		uint32_t BaseMip   = 0;
		uint32_t MipCount  = 1;
		uint32_t BaseLayer = 0;
		uint32_t LayerCount = 1;
	};

	// The "remaining subresources" sentinels (the Vulkan
	// VK_REMAINING_MIP_LEVELS / VK_REMAINING_ARRAY_LAYERS convention,
	// expressed backend-neutrally). An imported texture's true mip/layer
	// count is unknown to the RenderGraph, so its barriers cover
	// everything from the base level/layer down; the Vulkan backend passes
	// ~0u straight through to the subresourceRange.
	inline constexpr uint32_t kRemainingMipLevels   = ~0u;
	inline constexpr uint32_t kRemainingArrayLayers = ~0u;

	struct BufferBarrier
	{
		BufferHandle Buffer;
		BufferState  From = BufferState::Undefined;
		BufferState  To   = BufferState::Undefined;
		uint64_t     Offset = 0;
		uint64_t     Size   = ~0ull; // WHOLE_SIZE
	};

	struct BarrierDesc
	{
		TextureBarrier* Textures = nullptr;
		uint32_t        TextureCount = 0;
		BufferBarrier*  Buffers = nullptr;
		uint32_t        BufferCount = 0;
	};

	struct TimelineWait
	{
		uint64_t Value = 0;
	};

	struct TimelineSignal
	{
		uint64_t Value = 0;
	};

} // namespace hearth::rhi
