#include "tepch.h"
#include "Platform/Vulkan/Shaders/VulkanShaderReflection.h"
#include "hearth/Zone.h"

#include <spirv_cross.hpp>

namespace TinyEngine {

	VulkanShaderReflectionInfo VulkanShaderReflection::Reflect(const std::vector<uint32_t>& spirv)
	{
		HEARTH_ZONE_FUNCTION();

		spirv_cross::Compiler compiler(spirv);
		spirv_cross::ShaderResources resources = compiler.get_shader_resources();

		VulkanShaderReflectionInfo info;

		// Only one push constant block is supported right now (Vulkan itself
		// only allows one active range per stage combination in practice for
		// how this engine uses them), so the first one found is it.
		for (const spirv_cross::Resource& pushConstant : resources.push_constant_buffers)
		{
			const spirv_cross::SPIRType& type = compiler.get_type(pushConstant.base_type_id);
			info.PushConstantSize = (uint32_t)compiler.get_declared_struct_size(type);
			break;
		}

		for (const spirv_cross::Resource& sampler : resources.sampled_images)
		{
			info.HasCombinedImageSampler = true;
			info.CombinedImageSamplerBinding = compiler.get_decoration(sampler.id, spv::DecorationBinding);
			break;
		}

		return info;
	}

}
