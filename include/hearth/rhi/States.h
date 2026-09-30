#pragma once

// The RHI's state and queue vocabulary. The state->(layout, stage mask,
// access mask) table lives in the Vulkan implementation and is the only
// place layouts are named.

#include <cstdint>

namespace TinyEngine {
namespace rhi {

	enum class TextureState : uint8_t
	{
		Undefined,
		RenderTarget,
		DepthWrite,
		DepthRead,
		ShaderRead,
		TransferSrc,
		TransferDst,
		Present,
	};

	enum class BufferState : uint8_t
	{
		Undefined,
		VertexIndex,
		Uniform,
		Storage,
		TransferSrc,
		TransferDst,
		Indirect,
	};

	enum class QueueKind : uint8_t
	{
		Graphics,
		Transfer,
		Compute,
	};

} // namespace rhi
} // namespace TinyEngine
