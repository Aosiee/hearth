#pragma once

// The RHI's state and queue vocabulary. The state->(layout, stage mask,
// access mask) table lives in the Vulkan implementation and is the only
// place layouts are named.

#include <cstdint>

namespace hearth::rhi {

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
		// A swapchain image just acquired - its contents are undefined, and
		// its first barrier must chain with the acquire semaphore's wait
		// stage, so the source scope is that stage (the colour-attachment
		// output), not TOP_OF_PIPE. The swapchain's graph import state.
		SwapchainAcquired,
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
		// A compute shader's storage access: the precise source and
		// destination around a dispatch (Storage covers every shader stage).
		ComputeRead,
		ComputeWrite,
		// The CPU reads the buffer after the submission completes (a
		// readback): the device's writes made visible to the host.
		HostRead,
	};

	inline const char* BufferStateName(BufferState state)
	{
		switch (state)
		{
		case BufferState::Undefined:    return "Undefined";
		case BufferState::VertexIndex:  return "VertexIndex";
		case BufferState::Uniform:      return "Uniform";
		case BufferState::Storage:      return "Storage";
		case BufferState::TransferSrc:  return "TransferSrc";
		case BufferState::TransferDst:  return "TransferDst";
		case BufferState::Indirect:     return "Indirect";
		case BufferState::ComputeRead:  return "ComputeRead";
		case BufferState::ComputeWrite: return "ComputeWrite";
		case BufferState::HostRead:     return "HostRead";
		}
		return "Unknown";
	}

	// The shader stages a push-constant range covers (a pipeline's
	// PushConstantStages, a PushConstants call's stages).
	inline constexpr uint32_t kPushStageVertex = 1;
	inline constexpr uint32_t kPushStageFragment = 16;
	inline constexpr uint32_t kPushStageCompute = 32;

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

	// The colour-blend factors a pipeline can declare (added for the PBR
	// lighting passes' additive SceneColor accumulation; extended on demand).
	// AlphaBlend = SrcAlpha/OneMinusSrcAlpha, Additive = One/One.
	enum class BlendFactor : uint8_t
	{
		Zero,
		One,
		SrcAlpha,
		OneMinusSrcAlpha,
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
		case TextureState::SwapchainAcquired: return "SwapchainAcquired";
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

} // namespace hearth::rhi
