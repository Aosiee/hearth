#pragma once

// The Vulkan implementation of the RHI. Wrap-first: every handle wraps the
// existing Platform/Vulkan classes with no behaviour change. P-A1 ships
// the device lifecycle, the generation- checked handle tables, and the
// resource create/destroy surface; the frame-sync (P-A2) and bindless
// (P-A3) rewrites land on top.

#include "TinyEngine/RHI/RHI.h"

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanTexture.h"
#include "Platform/Vulkan/VulkanPipeline.h"
#include "Platform/Vulkan/VulkanShaderModule.h"
#include "Platform/Vulkan/VulkanSwapchain.h"

#include <memory>
#include <vector>

struct GLFWwindow;

namespace TinyEngine {
namespace rhi {

	// The standalone sampler (the existing VulkanTexture2D bakes its own
	// internally; this is the RHI's general form, consumed by the fixed
	// sampler table in P-A3).
	class VulkanSampler
	{
	public:
		void Init(VulkanDevice& device, const SamplerDesc& desc, const std::string& debugName);
		void Shutdown();
		VkSampler GetHandle() const;

	private:
		VulkanDevice* m_Device = nullptr;
		VkSampler m_Sampler = VK_NULL_HANDLE;
	};

	// The command-list implementation: wraps one VkCommandBuffer and
	// resolves every handle through the owning device.
	class VulkanRhiDevice;
	class VulkanCommandList final : public CommandList
	{
	public:
		VulkanCommandList(VulkanRhiDevice* device, VkCommandBuffer commandBuffer);

		void Begin() override;
		void End() override;
		void BeginRendering(std::span<const AttachmentDesc> attachments) override;
		void EndRendering() override;
		void Barrier(std::span<const BarrierDesc> barriers) override;
		void BindPipeline(PipelineHandle pipeline) override;
		void BindGlobalSet() override;
		void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) override;
		void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
		void PushConstants(uint32_t stages, uint32_t offset, const void* data, uint32_t size) override;
		void BindVertexBuffer(BufferHandle buffer, uint64_t offset = 0) override;
		void BindIndexBuffer(BufferHandle buffer, uint64_t offset = 0) override;
		void Draw(uint32_t vertexCount, uint32_t instanceCount = 1,
			uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;
		void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
			uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;
		void Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ) override;
		void CopyBufferToBuffer(BufferHandle src, BufferHandle dst,
			uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override;
		void CopyBufferToTexture(BufferHandle src, TextureHandle dst,
			uint32_t width, uint32_t height) override;
		void CopyTextureToBuffer(TextureHandle src, BufferHandle dst,
			uint32_t width, uint32_t height) override;
		void BeginLabel(const char* name) override;
		void EndLabel() override;
		void WriteTimestamp(uint32_t queryIndex) override;

		VkCommandBuffer GetCommandBuffer() const;

	private:
		VulkanRhiDevice* m_Device = nullptr;
		VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
		VkPipelineLayout m_BoundLayout = VK_NULL_HANDLE;
	};

	// P-D: a wrapped external image (WrapUIImageHandle) - the caller owns the
	// VkImage; the RHI owns only the view + the bindless slot. Destruction
	// retires the view + slot (no image, no sampler - the shared fixed
	// sampler must never enter a retire entry). The image itself is recorded
	// so barrier targets resolve through the same handle (P4-B: the
	// swapchain images are adopted this way).
	class VulkanWrappedTexture
	{
	public:
		VulkanWrappedTexture(uint32_t bindlessIndex, VkImageView view, VkImage image)
			: BindlessIndex(bindlessIndex), View(view), Image(image) {}

		uint32_t GetBindlessIndex() const { return BindlessIndex; }
		VkImageView GetView() const { return View; }
		VkImage GetImage() const { return Image; }

	private:
		uint32_t BindlessIndex = 0;
		VkImageView View = VK_NULL_HANDLE;
		VkImage Image = VK_NULL_HANDLE;
	};

	// One TextureHandle space: an entry is EITHER an owned VulkanTexture2D
	// or a wrapped external image (never both - two tables sharing the same
	// handle type would alias {index, generation} and mis-resolve).
	struct VulkanTextureEntry
	{
		std::unique_ptr<VulkanTexture2D> Owned;
		std::unique_ptr<VulkanWrappedTexture> Wrapped;

		bool IsWrapped() const { return Wrapped != nullptr; }
		uint32_t GetBindlessIndex() const
		{
			return Wrapped ? Wrapped->GetBindlessIndex()
				: (Owned ? Owned->GetBindlessIndex() : 0);
		}
	};

	class VulkanRhiDevice final : public Device
	{
	public:
		VulkanRhiDevice();
		~VulkanRhiDevice() override;

		// The device lifecycle: lazy on first window (the window is needed
		// for GLFW's instance-extension query), destroyed by the subsystem
		// on shutdown.
		void EnsureDevice(GLFWwindow* windowForExtensionQuery);
		void ShutdownDevice();
		VulkanDevice* GetVulkan() { return &m_Vulkan; }

		// ---- P-A3: the global bindless set + the retire queue ----------
		// One UPDATE_AFTER_BIND set per device; every texture gets a
		// stable BindlessIndex written into it at creation. Destroyed
		// resources are deferred through the retire queue until the
		// graphics timeline passes the submit that could reference them.
		static constexpr uint32_t kBindlessTextures = 16384;
		static constexpr uint32_t kBindlessBuffers = 4096;

		VkDescriptorSet GetGlobalDescriptorSet() const { return m_GlobalSet; }
		VkDescriptorSetLayout GetGlobalSetLayout() const { return m_GlobalSetLayout; }

		uint32_t AllocateBindlessTextureIndex();
		void WriteTextureDescriptor(uint32_t index, VkImageView imageView, VkSampler sampler, VkImageLayout layout);
		void QueueTextureForDestruction(VkImage image, VmaAllocation allocation,
			VkImageView imageView, VkSampler sampler, uint32_t bindlessIndex);
		void QueueBufferForDestruction(VkBuffer buffer, VmaAllocation allocation);
		void QueuePipelineForDestruction(VkPipeline pipeline, VkPipelineLayout layout);
		void QueueShaderForDestruction(VkShaderModule shader);
		void QueueSamplerForDestruction(VkSampler sampler);

		// Frees every entry whose stamp the GPU has passed (called once
		// per frame after submit). Shutdown drains unconditionally instead.
		void DrainRetireQueue();
		void DrainRetireQueueUnconditional();

		// Dev/test accessor: true while the index is handed out and not yet
		// recycled by a drain (the self-test's assertions).
		bool IsBindlessIndexLive(uint32_t index) const;

		// ---- Device -------------------------------------------------------
		BufferHandle CreateBuffer(const BufferDesc& desc, const char* debugName) override;
		void DestroyBuffer(BufferHandle handle) override;
		// P-C: borrows an engine-owned buffer (a Mesh's VulkanVertexBuffer/
		// VulkanIndexBuffer) as a non-owning handle so draws can go through
		// BufferHandles; DestroyBuffer on it releases the slot only.
		BufferHandle AdoptExternalBuffer(VkBuffer buffer, VmaAllocation allocation);
		void SetBufferData(BufferHandle handle, const void* data, uint64_t size, uint64_t offset = 0) override;
		void UploadBufferViaStaging(BufferHandle handle, const void* data, uint64_t size) override;

		TextureHandle CreateTexture(const TextureDesc& desc, const void* initialData, uint64_t dataSize, const char* debugName) override;
		TextureHandle CreateTextureFromBakedMips(uint32_t width, uint32_t height,
			const std::vector<std::vector<uint8_t>>& mipLevels, const char* debugName) override;
		TextureHandle CreateTextureFromNativeImage(void* nativeImage, uint32_t width, uint32_t height, Format format) override;
		TextureHandle AdoptExternalImage(void* nativeImage) override;
		void DestroyTexture(TextureHandle handle) override;
		void SetTextureData(TextureHandle handle, const void* data, uint64_t size) override;
		void UpdateTextureBakedMips(TextureHandle handle, const std::vector<std::vector<uint8_t>>& mipLevels) override;
		uint32_t GetBindlessIndex(TextureHandle handle) override;

		SamplerHandle CreateSampler(const SamplerDesc& desc, const char* debugName) override;
		void DestroySampler(SamplerHandle handle) override;

		ShaderHandle CreateShader(std::span<const uint32_t> spirv, ShaderStage stage, const char* debugName) override;
		void DestroyShader(ShaderHandle handle) override;

		PipelineHandle CreatePipeline(const PipelineStateDesc& desc, const char* debugName) override;
		void DestroyPipeline(PipelineHandle handle) override;

		CommandListHandle CreateCommandList(QueueHandle queue, uint32_t threadIndex) override;
		void DestroyCommandList(CommandListHandle handle) override;
		CommandList* GetCommandList(CommandListHandle handle) override;
		CommandList* AdoptCommandBuffer(void* commandBuffer) override;

		QueueHandle GetQueue(QueueKind kind) override;

		uint64_t Submit(QueueHandle queue, std::span<const CommandListHandle> commandLists,
			std::span<const TimelineWait> waits, std::span<const TimelineSignal> signals) override;

		void WaitIdle() override;

		SwapchainHandle CreateSwapchain(void* window) override;
		void DestroySwapchain(SwapchainHandle handle) override;

		// ---- Internal accessors the CommandList uses to resolve handles ----
		void SetClearColour(const float rgba[4])
		{
			for (int i = 0; i < 4; i++) m_ClearColour[i] = rgba[i];
		}
		const float* GetClearColour() const { return m_ClearColour; }
		VkBuffer GetBufferHandle(BufferHandle handle) const;
		VkImage GetTextureImage(TextureHandle handle) const;
		VkImageView GetTextureView(TextureHandle handle) const;
		VkExtent2D GetTextureSize(TextureHandle handle) const;
		VkPipeline GetPipelineHandle(PipelineHandle handle) const;
		VkPipelineLayout GetPipelineLayout(PipelineHandle handle) const;

	private:
		// P-A3: the global bindless set and the retire
		// queue. The stamp is the value the NEXT submit will signal
		// - the earliest submission that could reference the object.
		void CreateGlobalBindlessSet();
		void DestroyGlobalBindlessSet();
		void CreateFixedSamplers();

		struct RetireEntry
		{
			enum class Kind : uint8_t { Texture, Buffer, Pipeline, Shader, Sampler };

			Kind        Kind_ = Kind::Texture;
			uint64_t    Stamp = 0;
			VkImage     Image = VK_NULL_HANDLE;
			VmaAllocation Allocation = VK_NULL_HANDLE;
			VkImageView ImageView = VK_NULL_HANDLE;
			VkSampler   Sampler = VK_NULL_HANDLE;
			VkBuffer    Buffer = VK_NULL_HANDLE;
			VkPipeline  Pipeline = VK_NULL_HANDLE;
			VkPipelineLayout Layout = VK_NULL_HANDLE;
			VkShaderModule ShaderModule = VK_NULL_HANDLE;
			uint32_t    BindlessIndex = 0;
			bool        HasBindlessIndex = false;
		};
		void QueueRetire(RetireEntry&& entry);
		void FreeRetireEntry(const RetireEntry& entry);

		// The generation-checked handle table: fixed-slot objects + a
		// freelist; a freed slot bumps its generation. Deferred destruction
		// (the retire queue) arrives in P-A3 - P-A1 destroys synchronously.
		template <typename THandle, typename TObject>
		struct Table
		{
			struct Slot
			{
				std::unique_ptr<TObject> Object;
				uint32_t Generation = 0;
				bool Alive = false;
			};
			std::vector<Slot> Slots;
			std::vector<uint32_t> Free;

			THandle Acquire()
			{
				uint32_t index;
				if (!Free.empty())
				{
					index = Free.back();
					Free.pop_back();
				}
				else
				{
					index = (uint32_t)Slots.size();
					Slots.emplace_back();
				}
				Slot& slot = Slots[index];
				slot.Alive = true;
				slot.Object.reset();
				THandle handle;
				handle.Index = index;
				handle.Generation = slot.Generation;
				return handle;
			}

			bool Release(const THandle& handle)
			{
				if (handle.Index >= Slots.size() || !Slots[handle.Index].Alive)
					return false;
				if (Slots[handle.Index].Generation != handle.Generation)
					return false;
				Slot& slot = Slots[handle.Index];
				slot.Object.reset();
				slot.Alive = false;
				slot.Generation++;
				Free.push_back(handle.Index);
				return true;
			}

			TObject* Get(const THandle& handle) const
			{
				if (handle.Index >= Slots.size() || !Slots[handle.Index].Alive)
					return nullptr;
				if (Slots[handle.Index].Generation != handle.Generation)
					return nullptr;
				return Slots[handle.Index].Object.get();
			}

			void Clear()
			{
				for (Slot& slot : Slots)
					slot.Object.reset();
				Slots.clear();
				Free.clear();
			}
		};

		VulkanDevice m_Vulkan;
		bool m_Initialized = false;
		float m_ClearColour[4] = { 0.1f, 0.1f, 0.1f, 1.0f };

		Table<BufferHandle, VulkanBuffer> m_Buffers;
		Table<TextureHandle, VulkanTextureEntry> m_Textures;
		Table<SamplerHandle, VulkanSampler> m_Samplers;
		Table<ShaderHandle, VulkanShaderModule> m_Shaders;
		Table<PipelineHandle, VulkanPipeline> m_Pipelines;
		Table<CommandListHandle, VulkanCommandList> m_CommandLists;
		Table<SwapchainHandle, VulkanSwapchain> m_Swapchains;

		// P-D: the two fixed samplers Wisp picks from at texture creation
		// (baked with Wisp's exact parameters - single-level filtering).
		VkSampler m_FixedSamplerLinear = VK_NULL_HANDLE;
		VkSampler m_FixedSamplerNearest = VK_NULL_HANDLE;

		VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;
		VkDescriptorPool m_GlobalSetPool = VK_NULL_HANDLE;
		VkDescriptorSet m_GlobalSet = VK_NULL_HANDLE;

		std::vector<uint32_t> m_FreeBindlessIndices;
		uint32_t m_NextBindlessIndex = 0;
		std::vector<RetireEntry> m_RetireQueue;

		QueueHandle m_GraphicsQueue;
	};

} // namespace rhi

// P-E: the entry points for Platform/Vulkan code to reach the RHI impl
// (replaces the deleted VulkanDeviceRegistry shim). EnsureDevice is a
// no-op once the first window has created the device - callers that may
// run pre-window must go through the subsystem's EnsureDevice(window)
// directly (the helper asserts without a window).
rhi::VulkanRhiDevice& GetVulkanRhiDevice();

inline VulkanDevice& GetVulkanDevice()
{
	return *GetVulkanRhiDevice().GetVulkan();
}

} // namespace TinyEngine
