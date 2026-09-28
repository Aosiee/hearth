#pragma once

#include "Platform/Vulkan/VulkanCommon.h"

namespace TinyEngine {

	struct VulkanShaderReflectionInfo
	{
		uint32_t PushConstantSize = 0;

		bool HasCombinedImageSampler = false;
		uint32_t CombinedImageSamplerBinding = 0;
	};

	// SPIRV-Cross based reflection (vendored in the third_party plugin),
	// kept deliberately minimal: just enough to sanity check that the hand
	// declared descriptor set layout and push constant range in
	// VulkanDescriptor.h/VulkanPipeline.h actually match what a given shader
	// expects, rather than silently drawing with a mismatched layout and
	// getting garbage or a validation error at draw time instead of at load
	// time.
	class VulkanShaderReflection
	{
	public:
		static VulkanShaderReflectionInfo Reflect(const std::vector<uint32_t>& spirv);
	};

}
