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

	struct BufferDesc
	{
		uint64_t Size = 0;
		bool     HostVisible = false;
		bool     StorageUsage = false; // storage-buffer usage
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
		bool     PreserveDestinationAlpha = false;
		bool     DepthTestEnable = false;
		bool     DepthWriteEnable = false;
		CompareOp DepthCompareOp = CompareOp::Less;
		PolygonFillMode PolygonMode = PolygonFillMode::Fill;
		Format   ColorAttachmentFormat = Format::Undefined;
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
		virtual void DestroyBuffer(BufferHandle handle) = 0;
		virtual void SetBufferData(BufferHandle handle, const void* data, uint64_t size, uint64_t offset = 0) = 0;
		virtual void UploadBufferViaStaging(BufferHandle handle, const void* data, uint64_t size) = 0;

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

		// P-D: adopts a LIVE frame command buffer (begun by the frame loop)
		// as a CommandList for the calling layer to record into. No Begin/
		// End - the buffer is already recording. The caller deletes it.
		virtual CommandList* AdoptCommandBuffer(void* commandBuffer) = 0;

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
