#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDevice.h"

namespace TinyEngine {

	class VulkanDescriptorSetLayout
	{
	public:
		// One combined image sampler binding, nothing else. Just enough for
		// the single textured quad bring up test.
		void InitSingleCombinedImageSampler(VulkanDevice& device, VkShaderStageFlags stageFlags, const std::string& debugName);

		// The bindless-ish texture array Renderer2D's quad batch indexes by a
		// per-vertex TexIndex. This is hand declared policy, not something
		// reflection can infer (PARTIALLY_BOUND_BIT specifically, so slots
		// beyond whatever a given batch actually used don't need writing —
		// the shader only ever reads the slots the batch itself assigned).
		void InitTextureArray(VulkanDevice& device, uint32_t arrayCount, VkShaderStageFlags stageFlags, const std::string& debugName);

		void Shutdown();

		VkDescriptorSetLayout GetHandle() const { return m_Layout; }

	private:
		VulkanDevice* m_Device = nullptr;
		VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
	};

	// Growable descriptor pool - descriptorsPerSet defaults to 1 (a plain
	// combined image sampler set, the single-texture case); pass the array
	// size when allocating sets built from InitTextureArray instead.
	// initialMaxSets sizes the first internal pool block, not a hard ceiling:
	// Allocate() transparently creates another same-sized block ("pool
	// chaining", a standard Vulkan pattern) if the current one is exhausted,
	// rather than failing or requiring every caller to somehow never exceed a
	// fixed number. This used to be a genuinely fixed-size pool (one
	// VkDescriptorPool, created once) - callers that relied on that
	// (VulkanRenderer3D's own FrameSlot, at the time) had their own separate
	// fixed-size cap on top of it for the same reason; that layer's cap is
	// gone too, see its own comment in VulkanRenderer3D.cpp.
	class VulkanDescriptorAllocator
	{
	public:
		void Init(VulkanDevice& device, uint32_t initialMaxSets, const std::string& debugName, uint32_t descriptorsPerSet = 1);
		void Shutdown();

		VkDescriptorSet Allocate(VkDescriptorSetLayout layout, const std::string& debugName);

	private:
		VulkanDevice* m_Device = nullptr;
		std::vector<VkDescriptorPool> m_Pools;
		uint32_t m_MaxSetsPerPool = 0;
		uint32_t m_DescriptorsPerSet = 1;
		std::string m_DebugName;

		void CreatePoolBlock();
	};

	// Points a combined image sampler descriptor at a specific texture. layout
	// should be whatever VkImageLayout the image is actually in right now
	// (VulkanTexture2D::GetCurrentLayout), not assumed to always be
	// SHADER_READ_ONLY_OPTIMAL.
	void WriteCombinedImageSamplerDescriptor(VulkanDevice& device, VkDescriptorSet set, uint32_t binding, VkImageView imageView, VkSampler sampler, VkImageLayout layout);

	struct VulkanTextureArrayEntry
	{
		uint32_t ArrayIndex;
		VkImageView ImageView;
		VkSampler Sampler;
		VkImageLayout Layout;
	};

	// Writes one or more array elements of a texture-array binding (as created
	// by VulkanDescriptorSetLayout::InitTextureArray) in a single
	// vkUpdateDescriptorSets call.
	void WriteTextureArrayDescriptors(VulkanDevice& device, VkDescriptorSet set, uint32_t binding, const VulkanTextureArrayEntry* entries, uint32_t entryCount);

}
