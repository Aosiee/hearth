#include "hearth/rhi/vulkan/VulkanBuffer.h"
#include "hearth/Assert.h"
#include <cstring>

namespace hearth::rhi {

	void VulkanBuffer::Init(VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, const std::string& debugName)
	{
		m_Device = &device;
		m_Size = size;
		m_Usage = usage;

		VkBufferCreateInfo bufferCreateInfo{};
		bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCreateInfo.size = size;
		bufferCreateInfo.usage = usage;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocCreateInfo{};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

		if (hostVisible)
		{
			// MAPPED_BIT keeps this pointer valid for the buffer's entire
			// lifetime, no separate vmaMapMemory/UnmapMemory calls needed.
			allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		}

		VmaAllocationInfo allocationInfo{};
		VK_CHECK(vmaCreateBuffer(m_Device->GetAllocator(), &bufferCreateInfo, &allocCreateInfo, &m_Buffer, &m_Allocation, &allocationInfo));

		m_MappedData = hostVisible ? allocationInfo.pMappedData : nullptr;

		m_Device->SetObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)m_Buffer, debugName);
	}

	void VulkanBuffer::Shutdown()
	{
		if (m_Buffer != VK_NULL_HANDLE)
			vmaDestroyBuffer(m_Device->GetAllocator(), m_Buffer, m_Allocation);

		m_Buffer = VK_NULL_HANDLE;
		m_Allocation = VK_NULL_HANDLE;
		m_MappedData = nullptr;
		m_Usage = 0;
		m_Device = nullptr;
	}

	void VulkanBuffer::SetData(const void* data, VkDeviceSize size, VkDeviceSize offset)
	{
		HEARTH_ASSERT(m_MappedData, "VulkanBuffer::SetData called on a buffer that isn't host visible, use UploadViaStaging instead!");
		HEARTH_ASSERT(offset + size <= m_Size, "VulkanBuffer::SetData would write past the end of the buffer!");

		memcpy((uint8_t*)m_MappedData + offset, data, (size_t)size);
	}

	void VulkanBuffer::UploadViaStaging(const void* data, VkDeviceSize size)
	{
		HEARTH_ASSERT(size <= m_Size, "VulkanBuffer::UploadViaStaging would write past the end of the buffer!");

		VulkanBuffer staging;
		staging.Init(*m_Device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*hostVisible*/ true, "Staging Buffer (transient upload)");
		staging.SetData(data, size);

		VkCommandBuffer commandBuffer = m_Device->BeginSingleTimeCommands();

		VkBufferCopy copyRegion{};
		copyRegion.size = size;
		vkCmdCopyBuffer(commandBuffer, staging.GetHandle(), m_Buffer, 1, &copyRegion);

		m_Device->EndSingleTimeCommands(commandBuffer);

		// Inside an upload batch the recorded copy still references this
		// staging - hand it to the batch (it dies once the GPU passes the
		// batch's submission). Outside a batch, destroy it now exactly as
		// before.
		if (!m_Device->BatchRetireStaging(staging.GetHandle(), staging.GetAllocation()))
			staging.Shutdown();
	}

}
