#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/Device/VulkanDevice.h"
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
		// The source name it was created with (its debug name: the shader's
		// file name, "post.frag") - what a live reload matches on.
		const std::string& GetName() const { return m_Name; }

		// The in-place reload swaps the module and its SPIR-V with `other`
		// (a module built from the new code), so every handle to this one
		// stays valid; `other` is left holding the old module for the retire
		// queue.
		void SwapContents(VulkanShaderModule& other)
		{
			std::swap(m_Module, other.m_Module);
			std::swap(m_SpirV, other.m_SpirV);
		}

		// The asset-side identity (AssetID + variant key) for
		// InvalidatePipelines; HasIdentity false for unidentified shaders.
		rhi::ShaderIdentity Identity;
		bool HasIdentity = false;

	private:
		VulkanDevice* m_Device = nullptr;
		VkShaderModule m_Module = VK_NULL_HANDLE;
		VkShaderStageFlagBits m_Stage = VK_SHADER_STAGE_VERTEX_BIT;
		std::vector<uint32_t> m_SpirV;
		std::string m_Name;
	};

}
