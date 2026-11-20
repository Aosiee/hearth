#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/Device/VulkanDevice.h"

namespace TinyEngine {

	// Thin RAII wrapper around a VkBuffer and its VMA managed device memory.
	// This is a Vulkan implementation detail, engine code never touches it
	// directly: the RHI's buffer table holds one per BufferHandle (the
	// engine's VulkanVertexBuffer/VulkanIndexBuffer own such a handle), and a
	// few backend-internal staging/readback buffers use it directly.
	class VulkanBuffer
	{
	public:
		// hostVisible = true keeps the buffer in host visible, persistently
		// mapped memory, cheap to write to from the CPU every frame but slower
		// for the GPU to read (used by Renderer2D's per-frame vertex batch).
		// hostVisible = false puts the
		// buffer in fast device local memory, which has to be written through a
		// staging buffer (see UploadViaStaging) rather than mapped directly.
		void Init(VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, const std::string& debugName);
		void Shutdown();

		// Only valid on a host visible buffer, writes straight into the
		// persistently mapped pointer. There's no explicit flush call because
		// VMA's HOST_ACCESS_SEQUENTIAL_WRITE + the allocator's automatic
		// coherency handling (see Init) means a manual vkFlushMappedMemoryRanges
		// isn't needed for the access pattern this is used for.
		void SetData(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

		// Only valid on a device local buffer created with
		// VK_BUFFER_USAGE_TRANSFER_DST_BIT. Creates a temporary staging buffer,
		// copies data into it, then records and submits a one-shot command
		// buffer (see VulkanDevice::BeginSingleTimeCommands) to copy from the
		// staging buffer into this one.
		void UploadViaStaging(const void* data, VkDeviceSize size);

		VkBuffer GetHandle() const { return m_Buffer; }
		VkDeviceSize GetSize() const { return m_Size; }
		// The usage flags Init created it with (the RHI's staging-upload
		// assert reads TRANSFER_DST from it).
		VkBufferUsageFlags GetUsage() const { return m_Usage; }
		VmaAllocation GetAllocation() const { return m_Allocation; }
		// The host-visible staging's persistently-mapped pointer (the
		// IBL readback's memcpy source).
		void* GetMappedData() const { return m_MappedData; }

		// The buffer's slot in the bindless storage-buffer binding (1).
		// ~0u = none (the DestroyBuffer path passes it to the retire
		// queue, which then skips the tombstone).
		void SetBindlessIndex(uint32_t index) { m_BindlessIndex = index; }
		uint32_t GetBindlessIndex() const { return m_BindlessIndex; }

	private:
		VulkanDevice* m_Device = nullptr;
		VkBuffer m_Buffer = VK_NULL_HANDLE;
		VmaAllocation m_Allocation = VK_NULL_HANDLE;
		VkDeviceSize m_Size = 0;
		void* m_MappedData = nullptr;
		VkBufferUsageFlags m_Usage = 0;
		uint32_t m_BindlessIndex = ~0u;
	};

}
