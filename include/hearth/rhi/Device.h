#pragma once

// The Device surface. One per process, owned by the EngineScope
// `RhiDevice` subsystem. Wrap-first semantics: every create/destroy maps
// onto the existing Vulkan classes with no behaviour change; destroy is
// deferred through the retire queue once the persistent bindless set
// lands.

#include "TinyEngine/RHI/Types.h"
#include "TinyEngine/RHI/States.h"
#include "TinyEngine/RHI/Format.h"
#include "TinyEngine/RHI/BarrierDesc.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TinyEngine {
namespace rhi {

	class CommandList;

	// A buffer's explicit usage - CreateBuffer creates exactly the flags it is
	// given (no fixed superset). A device-local buffer implies TransferDst:
	// UploadBufferViaStaging is the only way to fill it.
	enum class BufferUsage : uint8_t
	{
		None        = 0,
		Vertex      = 1 << 0,
		Index       = 1 << 1,
		Storage     = 1 << 2,
		TransferDst = 1 << 3,
	};
	constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) { return (BufferUsage)((uint8_t)a | (uint8_t)b); }
	constexpr bool HasUsage(BufferUsage set, BufferUsage flag) { return ((uint8_t)set & (uint8_t)flag) != 0; }

	struct BufferDesc
	{
		uint64_t    Size = 0;
		bool        HostVisible = false;
		BufferUsage Usage = BufferUsage::None; // must be set (CreateBuffer asserts)
	};

	struct TextureDesc
	{
		uint32_t Width = 1;
		uint32_t Height = 1;
		uint32_t MipLevels = 1;
		Format   PixelFormat = Format::R8G8B8A8_UNORM;
		bool     RenderTarget = false; // colour-attachment usage
		bool     DepthTarget = false;  // depth-attachment usage
		SamplerKind Sampler = SamplerKind::Linear;

		// The TransientTexturePool's key-hit comparison (a changed desc
		// recreates the texture) - plain fieldwise equality.
		bool operator==(const TextureDesc& other) const
		{
			return Width == other.Width && Height == other.Height && MipLevels == other.MipLevels &&
				PixelFormat == other.PixelFormat && RenderTarget == other.RenderTarget &&
				DepthTarget == other.DepthTarget && Sampler == other.Sampler;
		}
		bool operator!=(const TextureDesc& other) const { return !(*this == other); }
	};

	struct SamplerDesc
	{
		bool Linear = true;
		bool Repeat = true;
		bool Anisotropy = false;
	};

	struct PipelineStateDesc
	{
		ShaderHandle VertexShader;
		ShaderHandle FragmentShader;

		// Vertex layout: stride + one entry per attribute (location, format,
		// offset).
		uint32_t VertexStride = 0;
		struct Attribute { uint32_t Location = 0; Format PixelFormat = Format::Undefined; uint32_t Offset = 0; };
		std::span<const Attribute> VertexAttributes;

		bool     EnableBlend = true;
		// The colour-blend factors when EnableBlend is true. The default is
		// the classic SrcAlpha/OneMinusSrcAlpha (every pre-existing pipeline
		// keeps its behaviour); the PBR lighting passes set One/One for
		// additive SceneColor accumulation.
		BlendFactor ColorBlendSrc = BlendFactor::SrcAlpha;
		BlendFactor ColorBlendDst = BlendFactor::OneMinusSrcAlpha;
		bool     PreserveDestinationAlpha = false;
		bool     DepthTestEnable = false;
		bool     DepthWriteEnable = false;
		CompareOp DepthCompareOp = CompareOp::Less;
		// The hardware slope-scaled depth bias (the local shadow caster's
		// rasterization state - the bias values are set dynamically via
		// vkCmdSetDepthBias).
		bool     DepthBiasEnable = false;
		float    DepthBiasConstantFactor = 0.0f;
		float    DepthBiasSlopeFactor = 0.0f;
		PolygonFillMode PolygonMode = PolygonFillMode::Fill;
		// An ARRAY of colour attachment formats (MRT for the G-buffer
		// pass); single-colour pipelines set count 1.
		static constexpr uint32_t kMaxColorAttachments = 8;
		Format   ColorAttachmentFormats[kMaxColorAttachments] = {};
		uint32_t ColorAttachmentCount = 1;
		Format   DepthAttachmentFormat = Format::Undefined;

		uint32_t PushConstantSize = 0;
		uint32_t PushConstantStages = 1; // vertex = 1; VERTEX|FRAGMENT = 1 | 16
	};

	enum class ShaderStage : uint8_t { Vertex, Fragment, Compute };

	class Device
	{
	public:
		virtual ~Device() = default;

		virtual BufferHandle CreateBuffer(const BufferDesc& desc, const char* debugName) = 0;
		// The handle is dead to its owner at once, but record-side
		// resolution continues until the retire frees it (in-flight draws
		// carry handles, not Refs) - a backend must not release the slot
		// immediately.
		virtual void DestroyBuffer(BufferHandle handle) = 0;
		virtual void SetBufferData(BufferHandle handle, const void* data, uint64_t size, uint64_t offset = 0) = 0;
		virtual void UploadBufferViaStaging(BufferHandle handle, const void* data, uint64_t size) = 0;
		// Bind a storage buffer into the bindless storage binding (binding
		// 1) at `index` (the per-frame/per-material SSBOs' descriptor write;
		// the handle remembers the slot so DestroyBuffer retires it).
		virtual void WriteBindlessBuffer(uint32_t index, BufferHandle handle) = 0;

		virtual TextureHandle CreateTexture(const TextureDesc& desc, const void* initialData, uint64_t dataSize, const char* debugName) = 0;
		// The baked-mip path: every level already carries final pixels (the
		// cooker baked them); one buffer->image copy per level, no blits.
		virtual TextureHandle CreateTextureFromBakedMips(uint32_t width, uint32_t height,
			const std::vector<std::vector<uint8_t>>& mipLevels, const char* debugName) = 0;
		// P-D: wraps an image the CALLER owns (a framebuffer's colour
		// attachment) as a samplable texture - the RHI owns only the view +
		// bindless slot; DestroyTexture releases just those.
		virtual TextureHandle CreateTextureFromNativeImage(void* nativeImage, uint32_t width, uint32_t height, Format format) = 0;
		// P4-B: adopts an externally-owned image as a barrier target only
		// (no view, no bindless slot - the swapchain images). Destroy
		// releases the handle; nothing is destroyed.
		virtual TextureHandle AdoptExternalImage(void* nativeImage) = 0;
		virtual void DestroyTexture(TextureHandle handle) = 0;
		virtual void SetTextureData(TextureHandle handle, const void* data, uint64_t size) = 0;
		virtual void UpdateTextureBakedMips(TextureHandle handle, const std::vector<std::vector<uint8_t>>& mipLevels) = 0;
		// The texture's slot in the global bindless set (the per-draw push).
		virtual uint32_t GetBindlessIndex(TextureHandle handle) = 0;

		virtual SamplerHandle CreateSampler(const SamplerDesc& desc, const char* debugName) = 0;
		virtual void DestroySampler(SamplerHandle handle) = 0;

		virtual ShaderHandle CreateShader(std::span<const uint32_t> spirv, ShaderStage stage, const char* debugName, const ShaderIdentity* identity = nullptr) = 0;
		virtual void DestroyShader(ShaderHandle handle) = 0;

		// Destroys + evicts every cached pipeline that references
		// a shader loaded from `shaderAsset` (shader hot reload calls this
		// before the new shader handles exist).
		virtual void InvalidatePipelines(const AssetID& shaderAsset) = 0;

		virtual PipelineHandle CreatePipeline(const PipelineStateDesc& desc, const char* debugName) = 0;
		virtual void DestroyPipeline(PipelineHandle handle) = 0;

		virtual CommandListHandle CreateCommandList(QueueHandle queue, uint32_t threadIndex) = 0;
		virtual void DestroyCommandList(CommandListHandle handle) = 0;
		virtual CommandList* GetCommandList(CommandListHandle handle) = 0;

		virtual QueueHandle GetQueue(QueueKind kind) = 0;

		// The upload batch. Uploads recorded between Begin and
		// End (the single-time-commands staging paths every loader uses)
		// are gathered into ONE submission at End - submitted without a
		// device waitIdle, so the frame that samples them is ordered after
		// them by the queue itself (the one-graphics-timeline invariant).
		// Game-thread-only (the promotion runs at the frame boundary);
		// End is a no-op submit if nothing recorded.
		virtual void BeginUploadBatch() = 0;
		virtual void EndUploadBatch() = 0;

		// v1: submits through the existing binary-semaphore/fence frame path;
		// timeline waits/signals become meaningful in P-A2.
		virtual uint64_t Submit(QueueHandle queue, std::span<const CommandListHandle> commandLists,
			std::span<const TimelineWait> waits, std::span<const TimelineSignal> signals) = 0;

		virtual void WaitIdle() = 0; // public use is dev-only; the subsystem idles internally on shutdown

		virtual SwapchainHandle CreateSwapchain(void* window) = 0; // P-A2 wires the per-window path
		virtual void DestroySwapchain(SwapchainHandle handle) = 0;
	};

} // namespace rhi
} // namespace TinyEngine
