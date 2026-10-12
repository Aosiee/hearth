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
		// A depth image sampled through the bindless set
		// (SHADER_READ_ONLY_OPTIMAL, the descriptor's layout) but
		// barrier-derived with the DEPTH aspect - distinct from
		// ShaderRead so the barrier code knows it is a depth image.
		DepthShaderRead,
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

	// The rasterization polygon mode a pipeline declares (added for the
	// wireframe debug view's line-rasterised pipeline).
	enum class PolygonFillMode : uint8_t
	{
		Fill,
		Line,
	};

	// A display name for a texture state (the GPU frame debugger's barrier
	// list uses the graph's vocabulary, not the backend's layout names).
	inline const char* TextureStateName(TextureState state)
	{
		switch (state)
		{
		case TextureState::Undefined:    return "Undefined";
		case TextureState::RenderTarget: return "RenderTarget";
		case TextureState::DepthWrite:   return "DepthWrite";
		case TextureState::DepthRead:    return "DepthRead";
		case TextureState::ShaderRead:   return "ShaderRead";
		case TextureState::DepthShaderRead: return "DepthShaderRead";
		case TextureState::TransferSrc:  return "TransferSrc";
		case TextureState::TransferDst:  return "TransferDst";
		case TextureState::Present:      return "Present";
		}
		return "Unknown";
	}

	// The small fixed sampler table a texture picks from at creation
	// (the per-texture sampler choice).
	enum class SamplerKind : uint8_t
	{
		Linear,
		Nearest,
	};

} // namespace rhi
} // namespace TinyEngine
