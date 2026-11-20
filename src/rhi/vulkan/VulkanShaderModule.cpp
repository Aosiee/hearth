#include "tepch.h"
#include "Platform/Vulkan/Shaders/VulkanShaderModule.h"

namespace TinyEngine {

	void VulkanShaderModule::Init(VulkanDevice& device, const std::vector<uint32_t>& spirv, VkShaderStageFlagBits stage, const std::string& debugName)
	{
		m_SpirV = spirv;
		m_Name = debugName;

		m_Device = &device;
		m_Stage = stage;

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = spirv.size() * sizeof(uint32_t);
		createInfo.pCode = spirv.data();

		VK_CHECK(vkCreateShaderModule(device.GetDevice(), &createInfo, nullptr, &m_Module));
		device.SetObjectName(VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)m_Module, debugName);
	}

	void VulkanShaderModule::Shutdown()
	{
		if (m_Module != VK_NULL_HANDLE)
			vkDestroyShaderModule(m_Device->GetDevice(), m_Module, nullptr);

		m_Module = VK_NULL_HANDLE;
	}

}
