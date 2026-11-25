#pragma once

// The handle model. Every RHI resource is a generation-checked
// handle: { index, generation }. The generation bumps when a slot is
// freed, so a stale handle fails the validity check instead of
// aliasing a new resource. Slots are never reused until the retire
// queue releases them.

#include <cstdint>


namespace hearth::rhi {

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

	// The identity of a shader's source, chosen by the program (its asset
	// id, say): 128 bits, all zero for none. Pipelines built from the source
	// are found by it (Device::InvalidatePipelines).
	struct ShaderSourceId
	{
		uint64_t High = 0;
		uint64_t Low = 0;
		bool operator==(const ShaderSourceId&) const = default;
	};

	// A shader's identity: its source and the permutation variant key (0 =
	// no variants). Stored on the shader entry.
	struct ShaderIdentity
	{
		ShaderSourceId Source;
		uint64_t VariantKey = 0;
	};
	struct PipelineHandle     : Handle {};
	struct CommandListHandle  : Handle {};
	struct SwapchainHandle    : Handle {};
	struct QueueHandle        : Handle {};

	inline bool IsValid(const Handle& handle) { return handle.Index != ~0u; }

} // namespace hearth::rhi
