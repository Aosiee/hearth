#pragma once

// The handle model. Every RHI resource is a generation-checked
// handle: { index, generation }. The generation bumps when a slot is
// freed, so a stale handle fails the validity check instead of
// aliasing a new resource. Slots are never reused until the retire
// queue releases them.

#include <cstdint>

#include "assetio/AssetID.h"

namespace TinyEngine {
namespace rhi {

	struct Handle
	{
		uint32_t Index = ~0u;
		uint32_t Generation = 0;
	};

	// Distinct types, not aliases: a BufferHandle must not be passable
	// where a TextureHandle is expected.
	struct BufferHandle       : Handle {};
	struct TextureHandle      : Handle {};
	struct SamplerHandle      : Handle {};
	struct ShaderHandle       : Handle {};

	// The asset-side identity of a shader (the AssetID it was
	// loaded from + the permutation variant key, 0 = no variants). Stored
	// on the shader entry; Device::InvalidatePipelines keys on it.
	struct ShaderIdentity
	{
		AssetID Asset;
		uint64_t VariantKey = 0;
	};
	struct PipelineHandle     : Handle {};
	struct CommandListHandle  : Handle {};
	struct SwapchainHandle    : Handle {};
	struct QueueHandle        : Handle {};

	inline bool IsValid(const Handle& handle) { return handle.Index != ~0u; }

} // namespace rhi
} // namespace TinyEngine
