#pragma once

#include "Platform/Vulkan/VulkanCommon.h"

namespace TinyEngine {

	struct VulkanShaderReflectionInfo
	{
		uint32_t PushConstantSize = 0;

		bool HasCombinedImageSampler = false;
		uint32_t CombinedImageSamplerBinding = 0;
	};

	// SPIRV-Cross based reflection (vendored under TinyEngine/vendor/SPIRV-Cross),
	// kept deliberately minimal for now: just enough to sanity check that the
	// hand declared descriptor set layout and push constant range in
	// VulkanDescriptor.h/VulkanPipeline.h actually match what a given shader
	// expects, rather than silently drawing with a mismatched layout and
	// getting garbage or a validation error at draw time instead of at load
	// time. A fuller reflection-driven DescriptorSetLayout builder (deriving
	// bindings automatically instead of hand declaring them) is later work,
	// not needed for this bring-up test.
	class VulkanShaderReflection
	{
	public:
		static VulkanShaderReflectionInfo Reflect(const std::vector<uint32_t>& spirv);
	};

}
