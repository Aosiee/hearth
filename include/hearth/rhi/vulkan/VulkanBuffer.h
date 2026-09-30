#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDevice.h"

namespace TinyEngine {

	// Thin RAII wrapper around a VkBuffer and its VMA managed device memory.
	// This is a Vulkan implementation detail, engine code never touches it
	// directly, VulkanVertexBuffer/VulkanIndexBuffer (and uniform/storage
	// buffers) are built on top of it.
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

		// P-C: a borrowed entry - the buffer (and its allocation) belong to
		// someone else (an engine Mesh's VulkanVertexBuffer/VulkanIndexBuffer);
		// Shutdown then only clears the borrow, never destroys.
		void Adopt(VkBuffer buffer, VmaAllocation allocation)
		{
			m_External = true;
			m_Buffer = buffer;
			m_Allocation = allocation;
		}
		bool IsExternal() const { return m_External; }

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
		VmaAllocation GetAllocation() const { return m_Allocation; }

	private:
		VulkanDevice* m_Device = nullptr;
		VkBuffer m_Buffer = VK_NULL_HANDLE;
		VmaAllocation m_Allocation = VK_NULL_HANDLE;
		VkDeviceSize m_Size = 0;
		void* m_MappedData = nullptr;
		bool m_External = false;
	};

}
