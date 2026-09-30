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

	// The depth compare operations a pipeline can declare (P-C added this
	// when the Renderer3D family ported; extended on demand).
	enum class CompareOp : uint8_t
	{
		Never,
		Less,
		LessOrEqual,
		Equal,
		GreaterOrEqual,
		Greater,
		Always,
	};

	// The small fixed sampler table a texture picks from at creation
	// (the per-texture sampler choice).
	enum class SamplerKind : uint8_t
	{
		Linear,
		Nearest,
	};

} // namespace rhi
} // namespace TinyEngine
