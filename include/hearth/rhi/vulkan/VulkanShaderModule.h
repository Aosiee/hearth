#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "TinyEngine/RHI/Types.h"

namespace TinyEngine {

	// Thin wrapper over a single VkShaderModule. Takes already compiled SPIR-V,
	// doesn't know or care how it got compiled, that's VulkanShaderCompiler's
	// job (glslang based). Deliberately decoupled from the compiler so this
	// class, and everything built on top of it (VulkanPipeline), stays
	// independent of how the SPIR-V was produced.
	class VulkanShaderModule
	{
	public:
		void Init(VulkanDevice& device, const std::vector<uint32_t>& spirv, VkShaderStageFlagBits stage, const std::string& debugName);
		void Shutdown();

		VkShaderModule GetHandle() const { return m_Module; }
		VkShaderStageFlagBits GetStage() const { return m_Stage; }
		const std::vector<uint32_t>& GetSpirV() const { return m_SpirV; }

		// The asset-side identity (AssetID + variant key) for
		// InvalidatePipelines; HasIdentity false for unidentified shaders.
		rhi::ShaderIdentity Identity;
		bool HasIdentity = false;

	private:
		VulkanDevice* m_Device = nullptr;
		VkShaderModule m_Module = VK_NULL_HANDLE;
		VkShaderStageFlagBits m_Stage = VK_SHADER_STAGE_VERTEX_BIT;
		std::vector<uint32_t> m_SpirV;
	};

}
