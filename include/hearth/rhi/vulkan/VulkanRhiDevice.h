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

		// ---- Device -------------------------------------------------------
		BufferHandle CreateBuffer(const BufferDesc& desc, const char* debugName) override;
		void DestroyBuffer(BufferHandle handle) override;
		void SetBufferData(BufferHandle handle, const void* data, uint64_t size, uint64_t offset = 0) override;
		void UploadBufferViaStaging(BufferHandle handle, const void* data, uint64_t size) override;

		TextureHandle CreateTexture(const TextureDesc& desc, const void* initialData, uint64_t dataSize, const char* debugName) override;
		TextureHandle CreateTextureFromBakedMips(uint32_t width, uint32_t height,
			const std::vector<std::vector<uint8_t>>& mipLevels, const char* debugName) override;
		void DestroyTexture(TextureHandle handle) override;
		void SetTextureData(TextureHandle handle, const void* data, uint64_t size) override;
		void UpdateTextureBakedMips(TextureHandle handle, const std::vector<std::vector<uint8_t>>& mipLevels) override;

		SamplerHandle CreateSampler(const SamplerDesc& desc, const char* debugName) override;
		void DestroySampler(SamplerHandle handle) override;

		ShaderHandle CreateShader(std::span<const uint32_t> spirv, ShaderStage stage, const char* debugName) override;
		void DestroyShader(ShaderHandle handle) override;

		PipelineHandle CreatePipeline(const PipelineStateDesc& desc, const char* debugName) override;
		void DestroyPipeline(PipelineHandle handle) override;

		CommandListHandle CreateCommandList(QueueHandle queue, uint32_t threadIndex) override;
		void DestroyCommandList(CommandListHandle handle) override;
		CommandList* GetCommandList(CommandListHandle handle) override;

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
		Table<TextureHandle, VulkanTexture2D> m_Textures;
		Table<SamplerHandle, VulkanSampler> m_Samplers;
		Table<ShaderHandle, VulkanShaderModule> m_Shaders;
		Table<PipelineHandle, VulkanPipeline> m_Pipelines;
		Table<CommandListHandle, VulkanCommandList> m_CommandLists;
		Table<SwapchainHandle, VulkanSwapchain> m_Swapchains;

		QueueHandle m_GraphicsQueue;
	};

} // namespace rhi
} // namespace TinyEngine
