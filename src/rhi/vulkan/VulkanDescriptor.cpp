#include "tepch.h"
#include "Platform/Vulkan/Resources/VulkanDescriptor.h"
#include "hearth/Log.h"

namespace TinyEngine {

	void VulkanDescriptorSetLayout::InitSingleCombinedImageSampler(VulkanDevice& device, VkShaderStageFlags stageFlags, const std::string& debugName)
	{
		m_Device = &device;

		VkDescriptorSetLayoutBinding binding{};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = stageFlags;

		VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
		layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutCreateInfo.bindingCount = 1;
		layoutCreateInfo.pBindings = &binding;

		VK_CHECK(vkCreateDescriptorSetLayout(device.GetDevice(), &layoutCreateInfo, nullptr, &m_Layout));
		device.SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, (uint64_t)m_Layout, debugName);
	}

	void VulkanDescriptorSetLayout::InitTextureArray(VulkanDevice& device, uint32_t arrayCount, VkShaderStageFlags stageFlags, const std::string& debugName)
	{
		m_Device = &device;

		VkDescriptorSetLayoutBinding binding{};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = arrayCount;
		binding.stageFlags = stageFlags;

		// Lets a batch that only used, say, 3 of the 32 slots leave the other
		// 29 unwritten without that being a validation error — the shader only
		// ever reads the slots a given TexIndex value actually references,
		// which a batch never assigns beyond how many textures it used.
		VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCreateInfo{};
		bindingFlagsCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		bindingFlagsCreateInfo.bindingCount = 1;
		bindingFlagsCreateInfo.pBindingFlags = &bindingFlags;

		VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
		layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutCreateInfo.pNext = &bindingFlagsCreateInfo;
		layoutCreateInfo.bindingCount = 1;
		layoutCreateInfo.pBindings = &binding;

		VK_CHECK(vkCreateDescriptorSetLayout(device.GetDevice(), &layoutCreateInfo, nullptr, &m_Layout));
		device.SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, (uint64_t)m_Layout, debugName);
	}

	void VulkanDescriptorSetLayout::Shutdown()
	{
		if (m_Layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(m_Device->GetDevice(), m_Layout, nullptr);

		m_Layout = VK_NULL_HANDLE;
	}

	void VulkanDescriptorAllocator::Init(VulkanDevice& device, uint32_t initialMaxSets, const std::string& debugName, uint32_t descriptorsPerSet)
	{
		m_Device = &device;
		m_MaxSetsPerPool = initialMaxSets;
		m_DescriptorsPerSet = descriptorsPerSet;
		m_DebugName = debugName;

		CreatePoolBlock();
	}

	void VulkanDescriptorAllocator::CreatePoolBlock()
	{
		VkDescriptorPoolSize poolSize{};
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = m_MaxSetsPerPool * m_DescriptorsPerSet;

		VkDescriptorPoolCreateInfo poolCreateInfo{};
		poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolCreateInfo.maxSets = m_MaxSetsPerPool;
		poolCreateInfo.poolSizeCount = 1;
		poolCreateInfo.pPoolSizes = &poolSize;

		VkDescriptorPool pool;
		VK_CHECK(vkCreateDescriptorPool(m_Device->GetDevice(), &poolCreateInfo, nullptr, &pool));
		m_Device->SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, (uint64_t)pool, m_DebugName + " (block " + std::to_string(m_Pools.size()) + ")");

		m_Pools.push_back(pool);
	}

	void VulkanDescriptorAllocator::Shutdown()
	{
		for (VkDescriptorPool pool : m_Pools)
			vkDestroyDescriptorPool(m_Device->GetDevice(), pool, nullptr);

		m_Pools.clear();
	}

	VkDescriptorSet VulkanDescriptorAllocator::Allocate(VkDescriptorSetLayout layout, const std::string& debugName)
	{
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = m_Pools.back();
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &layout;

		VkDescriptorSet set;
		VkResult result = vkAllocateDescriptorSets(m_Device->GetDevice(), &allocInfo, &set);

		// The current pool block is exhausted (its own fixed maxSets, set
		// at CreatePoolBlock time) - grow by adding another same-sized
		// block rather than failing outright or requiring a caller to
		// never exceed a fixed number. Every other VkResult still goes
		// through VK_CHECK below and asserts - this only special-cases the
		// two "pool's out of room" results.
		if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
		{
			HEARTH_LOG_INFO(::hearth::LogVulkan, "VulkanDescriptorAllocator '{0}': pool block {1} exhausted, growing to block {2}.", m_DebugName, m_Pools.size() - 1, m_Pools.size());

			CreatePoolBlock();
			allocInfo.descriptorPool = m_Pools.back();
			result = vkAllocateDescriptorSets(m_Device->GetDevice(), &allocInfo, &set);
		}

		VK_CHECK(result);
		m_Device->SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, (uint64_t)set, debugName);

		return set;
	}

	void WriteCombinedImageSamplerDescriptor(VulkanDevice& device, VkDescriptorSet set, uint32_t binding, VkImageView imageView, VkSampler sampler, VkImageLayout layout)
	{
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = layout;
		imageInfo.imageView = imageView;
		imageInfo.sampler = sampler;

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = set;
		write.dstBinding = binding;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;

		vkUpdateDescriptorSets(device.GetDevice(), 1, &write, 0, nullptr);
	}

	void WriteTextureArrayDescriptors(VulkanDevice& device, VkDescriptorSet set, uint32_t binding, const VulkanTextureArrayEntry* entries, uint32_t entryCount)
	{
		if (entryCount == 0)
			return;

		std::vector<VkDescriptorImageInfo> imageInfos(entryCount);
		std::vector<VkWriteDescriptorSet> writes(entryCount);

		for (uint32_t i = 0; i < entryCount; i++)
		{
			imageInfos[i].imageLayout = entries[i].Layout;
			imageInfos[i].imageView = entries[i].ImageView;
			imageInfos[i].sampler = entries[i].Sampler;

			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = set;
			writes[i].dstBinding = binding;
			writes[i].dstArrayElement = entries[i].ArrayIndex;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &imageInfos[i];
		}

		vkUpdateDescriptorSets(device.GetDevice(), entryCount, writes.data(), 0, nullptr);
	}

}
