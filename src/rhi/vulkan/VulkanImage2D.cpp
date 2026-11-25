#include "hearth/rhi/vulkan/VulkanImage2D.h"
#include "hearth/rhi/vulkan/VulkanBuffer.h"
#include "hearth/rhi/vulkan/VulkanDevice.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"
#include "hearth/Zone.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace hearth::rhi {

	static uint32_t CalculateMipCount(uint32_t width, uint32_t height)
	{
		return (uint32_t)std::floor(std::log2((double)std::max(width, height))) + 1;
	}

	VulkanImage2D::VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, bool srgb)
		: m_RhiDevice(&rhiDevice), m_Srgb(srgb)
	{
		HEARTH_ZONE_FUNCTION();

		CreateImage(width, height);

		// No pixels yet (the image is filled later through SetData): upload
		// zeroes rather than leave the contents undefined until then.
		std::vector<uint8_t> blank((size_t)width * height * 4, 0);
		UploadAndGenerateMips(blank.data());
	}

	VulkanImage2D::VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, std::span<const uint8_t> rgba8, bool srgb)
		: m_RhiDevice(&rhiDevice), m_Srgb(srgb)
	{
		HEARTH_ZONE_FUNCTION();

		HEARTH_ASSERT(rgba8.size() == (size_t)width * height * 4, "VulkanImage2D: the pixels are not width * height RGBA8");
		CreateImage(width, height);
		UploadAndGenerateMips(rgba8.data());
	}

	VulkanImage2D::VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>>& mipLevels, uint32_t firstMipCount)
		: m_RhiDevice(&rhiDevice)
	{
		HEARTH_ZONE_FUNCTION();

		HEARTH_ASSERT(!mipLevels.empty(), "VulkanImage2D: a baked mip chain needs at least one level (the full resolution image)!");
		HEARTH_ASSERT(firstMipCount == 0 || firstMipCount <= mipLevels.size(), "VulkanImage2D: firstMipCount exceeds the baked mip chain!");

		// The image is ALWAYS created with the full cooked mip count (so
		// the object never changes shape during streaming); only how many
		// levels hold real pixels differs.
		CreateImage(width, height, (uint32_t)mipLevels.size());

		if (firstMipCount == 0 || firstMipCount >= mipLevels.size())
			UpdateBakedMips(width, height, mipLevels);
		else
			UploadPrefixAndFillMips(mipLevels, firstMipCount);
	}

	VulkanImage2D::VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, VkFormat format, bool depthTarget)
		: m_RhiDevice(&rhiDevice)
	{
		HEARTH_ZONE_FUNCTION();

		CreateImageRenderTarget(width, height, format, depthTarget);
		CreateSampler();
		AcquireBindlessSlot();
	}

	void VulkanImage2D::CreateImageRenderTarget(uint32_t width, uint32_t height, VkFormat format, bool depthTarget)
	{
		m_Width = width;
		m_Height = height;
		m_MipLevels = 1;
		m_Format = format;

		VulkanDevice& device = *m_RhiDevice->GetVulkan();

		// The format-feature check the render-target path needs (the
		// texture path only asserts SAMPLED-linear; R16G16_UNORM etc. are
		// NOT mandatory colour-attachment formats, so an exotic driver must
		// fail loudly rather than render garbage).
		VkFormatProperties formatProperties;
		vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), m_Format, &formatProperties);
		if (depthTarget)
		{
			HEARTH_ASSERT(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
				"render-target depth format without DEPTH_STENCIL_ATTACHMENT on this GPU");
		}
		else
		{
			HEARTH_ASSERT(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
				"render-target colour format without COLOR_ATTACHMENT on this GPU");
		}

		VkImageCreateInfo imageCreateInfo{};
		imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.format = m_Format;
		imageCreateInfo.extent = { width, height, 1 };
		imageCreateInfo.mipLevels = 1;
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		// SAMPLED: the lighting/post passes read the target back through the
		// bindless set. TRANSFER_SRC: a render target can be read back (the
		// tool flavour's offscreen UI capture).
		imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
			| (depthTarget ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VmaAllocationCreateInfo allocCreateInfo{};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VK_CHECK(vmaCreateImage(device.GetAllocator(), &imageCreateInfo, &allocCreateInfo, &m_Image, &m_Allocation, nullptr));
		device.SetObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)m_Image, depthTarget ? "Render Target Depth" : "Render Target Colour");

		VkImageViewCreateInfo viewCreateInfo{};
		viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewCreateInfo.image = m_Image;
		viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewCreateInfo.format = m_Format;
		viewCreateInfo.subresourceRange.aspectMask = depthTarget ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		viewCreateInfo.subresourceRange.baseMipLevel = 0;
		viewCreateInfo.subresourceRange.levelCount = 1;
		viewCreateInfo.subresourceRange.baseArrayLayer = 0;
		viewCreateInfo.subresourceRange.layerCount = 1;

		VK_CHECK(vkCreateImageView(device.GetDevice(), &viewCreateInfo, nullptr, &m_ImageView));
		device.SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)m_ImageView, depthTarget ? "Render Target Depth View" : "Render Target Colour View");
	}

	VulkanImage2D::~VulkanImage2D()
	{
		HEARTH_ZONE_FUNCTION();

		// Deferred destruction: an in-flight frame may still be
		// sampling this texture through its bindless slot, so the Vk objects
		// and the slot's recycle both wait for the retire queue to observe
		// the graphics timeline pass the next submit.
		if (m_Image != VK_NULL_HANDLE)
			m_RhiDevice->QueueTextureForDestruction(m_Image, m_Allocation, m_ImageView, m_Sampler, m_BindlessIndex);
		// The sRGB view's own entry retires too (a second entry carrying
		// only the view + its bindless slot - the image/allocation are
		// already queued above).
		if (m_ImageViewSrgb != VK_NULL_HANDLE)
			m_RhiDevice->QueueTextureForDestruction(VK_NULL_HANDLE, VK_NULL_HANDLE, m_ImageViewSrgb, VK_NULL_HANDLE, m_BindlessIndexSrgb);
	}

	void VulkanImage2D::CreateImage(uint32_t width, uint32_t height, uint32_t mipCount)
	{
		m_Width = width;
		m_Height = height;
		m_MipLevels = mipCount != 0 ? mipCount : CalculateMipCount(width, height);

		VulkanDevice& device = *m_RhiDevice->GetVulkan();

		VkFormatProperties formatProperties;
		vkGetPhysicalDeviceFormatProperties(device.GetPhysicalDevice(), m_Format, &formatProperties);
		HEARTH_ASSERT(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
			"This GPU can't linear filter blit R8G8B8A8_UNORM, the manual mipmap chain below would produce garbage on it!");

		VkImageCreateInfo imageCreateInfo{};
		imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.format = m_Format;
		imageCreateInfo.extent = { width, height, 1 };
		imageCreateInfo.mipLevels = m_MipLevels;
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		// TRANSFER_SRC is needed because every mip level except the last is read
		// from as a blit source while generating the chain below.
		imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		// An sRGB texture's image is created mutable-format with a format
		// list naming exactly the two formats the views will use (keeps
		// delta-colour compression on GPUs that would otherwise disable it
		// for an open-ended view set).
		VkImageFormatListCreateInfo formatList{};
		if (m_Srgb)
		{
			const VkFormat kSrgbViewFormats[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
			formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
			formatList.viewFormatCount = 2;
			formatList.pViewFormats = kSrgbViewFormats;
			imageCreateInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			imageCreateInfo.pNext = &formatList;
		}

		VmaAllocationCreateInfo allocCreateInfo{};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VK_CHECK(vmaCreateImage(device.GetAllocator(), &imageCreateInfo, &allocCreateInfo, &m_Image, &m_Allocation, nullptr));
		device.SetObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)m_Image, "Texture2D Image");

		VkImageViewCreateInfo viewCreateInfo{};
		viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewCreateInfo.image = m_Image;
		viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewCreateInfo.format = m_Format;
		viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewCreateInfo.subresourceRange.baseMipLevel = 0;
		viewCreateInfo.subresourceRange.levelCount = m_MipLevels;
		viewCreateInfo.subresourceRange.baseArrayLayer = 0;
		viewCreateInfo.subresourceRange.layerCount = 1;

		VK_CHECK(vkCreateImageView(device.GetDevice(), &viewCreateInfo, nullptr, &m_ImageView));
		device.SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)m_ImageView, "Texture2D Image View");

		// The second `_SRGB` view for an sRGB-flagged texture (the
		// hardware decode entry - the UNORM view above stays the UI/2D
		// passthrough entry).
		if (m_Srgb)
		{
			VkImageViewCreateInfo srgbViewCreateInfo = viewCreateInfo;
			srgbViewCreateInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
			VK_CHECK(vkCreateImageView(device.GetDevice(), &srgbViewCreateInfo, nullptr, &m_ImageViewSrgb));
			device.SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)m_ImageViewSrgb, "Texture2D sRGB Image View");
		}

		CreateSampler();
		AcquireBindlessSlot();
	}

	void VulkanImage2D::AcquireBindlessSlot()
	{
		VulkanRhiDevice& rhiDevice = *m_RhiDevice;
		m_BindlessIndex = rhiDevice.AllocateBindlessTextureIndex();

		// The descriptor's imageLayout is a promise about the layout at DRAW
		// time, not at write time: every upload path leaves this image in
		// SHADER_READ_ONLY_OPTIMAL, the only layout it is ever sampled in .
		rhiDevice.WriteTextureDescriptor(m_BindlessIndex, m_ImageView, m_Sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// The second bindless entry for the sRGB view; linear textures
		// alias the one entry.
		if (m_Srgb)
		{
			m_BindlessIndexSrgb = rhiDevice.AllocateBindlessTextureIndex();
			rhiDevice.WriteTextureDescriptor(m_BindlessIndexSrgb, m_ImageViewSrgb, m_Sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		else
		{
			m_BindlessIndexSrgb = m_BindlessIndex;
		}
	}

	void VulkanImage2D::CreateSampler()
	{
		VulkanDevice& device = *m_RhiDevice->GetVulkan();

		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(device.GetPhysicalDevice(), &properties);

		VkSamplerCreateInfo samplerCreateInfo{};
		samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		// Trilinear filtering.
		samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
		samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
		samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		// Anisotropic filtering is an optional device feature (see
		// VulkanDevice::IsSamplerAnisotropySupported), requesting it without the
		// feature enabled at device creation is a validation error, not just a
		// silently ignored setting.
		samplerCreateInfo.anisotropyEnable = device.IsSamplerAnisotropySupported() ? VK_TRUE : VK_FALSE;
		samplerCreateInfo.maxAnisotropy = device.IsSamplerAnisotropySupported() ? properties.limits.maxSamplerAnisotropy : 1.0f;
		samplerCreateInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerCreateInfo.unnormalizedCoordinates = VK_FALSE;
		samplerCreateInfo.compareEnable = VK_FALSE;
		samplerCreateInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerCreateInfo.minLod = 0.0f;
		samplerCreateInfo.maxLod = (float)m_MipLevels;
		samplerCreateInfo.mipLodBias = 0.0f;

		VK_CHECK(vkCreateSampler(device.GetDevice(), &samplerCreateInfo, nullptr, &m_Sampler));
	}

	void VulkanImage2D::UploadAndGenerateMips(const void* pixels)
	{
		VulkanDevice& device = *m_RhiDevice->GetVulkan();
		VkDeviceSize imageSize = (VkDeviceSize)m_Width * m_Height * 4;

		VulkanBuffer staging;
		staging.Init(device, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*hostVisible*/ true, "Texture Upload Staging Buffer");
		staging.SetData(pixels, imageSize);

		VkCommandBuffer cmd = device.BeginSingleTimeCommands();

		// Whichever layout this image is currently in (UNDEFINED the first time,
		// SHADER_READ_ONLY_OPTIMAL on every re-upload after that via SetData) ->
		// TRANSFER_DST_OPTIMAL, across the whole mip chain, so we can copy into
		// level 0 and then blit into every level above it.
		bool firstUpload = m_CurrentLayout == VK_IMAGE_LAYOUT_UNDEFINED;

		VkImageMemoryBarrier2 toDst{};
		toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		toDst.srcStageMask = firstUpload ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		toDst.srcAccessMask = firstUpload ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_READ_BIT;
		toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		toDst.oldLayout = m_CurrentLayout;
		toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image = m_Image;
		toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_MipLevels, 0, 1 };

		VkDependencyInfo toDstDep{};
		toDstDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toDstDep.imageMemoryBarrierCount = 1;
		toDstDep.pImageMemoryBarriers = &toDst;
		vkCmdPipelineBarrier2(cmd, &toDstDep);

		VkBufferImageCopy copyRegion{};
		copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyRegion.imageExtent = { m_Width, m_Height, 1 };
		vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

		// Manual mip chain: there's no Vulkan equivalent of
		// glGenerateTextureMipmap, so each level is blitted down from the one
		// before it. Every source level has to move from TRANSFER_DST_OPTIMAL
		// (freshly written, either by the copy above for level 0 or by the
		// previous iteration's blit) into TRANSFER_SRC_OPTIMAL before it can be
		// read from as a blit source.
		int32_t mipWidth = (int32_t)m_Width;
		int32_t mipHeight = (int32_t)m_Height;

		for (uint32_t level = 1; level < m_MipLevels; level++)
		{
			VkImageMemoryBarrier2 srcReady{};
			srcReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			srcReady.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			srcReady.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			srcReady.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			srcReady.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			srcReady.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			srcReady.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			srcReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			srcReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			srcReady.image = m_Image;
			srcReady.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1 };

			VkDependencyInfo srcReadyDep{};
			srcReadyDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			srcReadyDep.imageMemoryBarrierCount = 1;
			srcReadyDep.pImageMemoryBarriers = &srcReady;
			vkCmdPipelineBarrier2(cmd, &srcReadyDep);

			int32_t nextWidth = mipWidth > 1 ? mipWidth / 2 : 1;
			int32_t nextHeight = mipHeight > 1 ? mipHeight / 2 : 1;

			VkImageBlit blit{};
			blit.srcOffsets[0] = { 0, 0, 0 };
			blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
			blit.dstOffsets[0] = { 0, 0, 0 };
			blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };

			vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

			mipWidth = nextWidth;
			mipHeight = nextHeight;
		}

		// Levels [0, m_MipLevels - 2] ended the loop above in
		// TRANSFER_SRC_OPTIMAL (each was read from as a blit source at some
		// point). The very last level only ever got written into, it's still in
		// TRANSFER_DST_OPTIMAL. Two barriers because one barrier can't describe
		// two different "old" layouts within a single subresource range.
		std::vector<VkImageMemoryBarrier2> finalBarriers;

		if (m_MipLevels > 1)
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = m_Image;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_MipLevels - 1, 0, 1 };
			finalBarriers.push_back(barrier);
		}

		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = m_Image;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m_MipLevels - 1, 1, 0, 1 };
			finalBarriers.push_back(barrier);
		}

		VkDependencyInfo finalDep{};
		finalDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		finalDep.imageMemoryBarrierCount = (uint32_t)finalBarriers.size();
		finalDep.pImageMemoryBarriers = finalBarriers.data();
		vkCmdPipelineBarrier2(cmd, &finalDep);

		device.EndSingleTimeCommands(cmd);

		// Inside an upload batch the recorded copies still reference this
		// staging - hand it to the batch (it dies once the GPU passes the
		// batch's submission). Outside a batch, destroy it now.
		if (!device.BatchRetireStaging(staging.GetHandle(), staging.GetAllocation()))
			staging.Shutdown();

		m_CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	void VulkanImage2D::UpdateBakedMips(uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>>& mipLevels)
	{
		VulkanDevice& device = *m_RhiDevice->GetVulkan();

		// Same-source re-upload only: the
		// image/view/sampler stay valid, so dims and mip count must match
		// what CreateImage allocated (a re-cook of the same source always
		// produces the same dims; a different source is a different texture
		// object).
		HEARTH_ASSERT(width == m_Width && height == m_Height, "VulkanImage2D: UpdateBakedMips dims don't match the allocated image - a re-cook of the same source always keeps its dimensions!");
		HEARTH_ASSERT(mipLevels.size() == m_MipLevels, "VulkanImage2D: baked mip level count doesn't match what CreateImage was told to allocate!");

		// Each level must carry exactly levelWidth*levelHeight*4 bytes, or
		// the per-level copy offsets below would silently misalign.
		uint32_t checkWidth = m_Width;
		uint32_t checkHeight = m_Height;
		for (uint32_t level = 0; level < m_MipLevels; level++)
		{
			HEARTH_ASSERT(mipLevels[level].size() == (size_t)checkWidth * checkHeight * 4, "VulkanImage2D: baked mip level byte size doesn't match its dimensions!");
			checkWidth = checkWidth > 1 ? checkWidth / 2 : 1;
			checkHeight = checkHeight > 1 ? checkHeight / 2 : 1;
		}

		// One staging buffer sized for the whole chain (level 0 dominates the
		// total size - each level below it is a quarter the size of the one
		// before, so the geometric series only adds about a third more on
		// top of level 0 alone), rather than one staging buffer per level.
		VkDeviceSize totalSize = 0;
		for (const std::vector<uint8_t>& level : mipLevels)
			totalSize += level.size();

		VulkanBuffer staging;
		staging.Init(device, totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*hostVisible*/ true, "Baked Mip Upload Staging Buffer");

		std::vector<VkDeviceSize> levelOffsets(mipLevels.size());
		VkDeviceSize writeOffset = 0;
		for (size_t level = 0; level < mipLevels.size(); level++)
		{
			levelOffsets[level] = writeOffset;
			staging.SetData(mipLevels[level].data(), mipLevels[level].size(), writeOffset);
			writeOffset += mipLevels[level].size();
		}

		VkCommandBuffer cmd = device.BeginSingleTimeCommands();

		// Pre-barrier: fresh-construction uploads start from UNDEFINED; a
		// hot-reload re-upload starts from whatever the image currently is
		// (SHADER_READ_ONLY_OPTIMAL) and must keep the prior sampling reads
		// ordered before the transfer write - the same firstUpload branch
		// UploadAndGenerateMips already uses.
		const bool firstUpload = (m_CurrentLayout == VK_IMAGE_LAYOUT_UNDEFINED);

		VkImageMemoryBarrier2 toDst{};
		toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		toDst.srcStageMask = firstUpload ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		toDst.srcAccessMask = firstUpload ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_READ_BIT;
		toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		toDst.oldLayout = m_CurrentLayout;
		toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image = m_Image;
		toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_MipLevels, 0, 1 };

		VkDependencyInfo toDstDep{};
		toDstDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toDstDep.imageMemoryBarrierCount = 1;
		toDstDep.pImageMemoryBarriers = &toDst;
		vkCmdPipelineBarrier2(cmd, &toDstDep);

		uint32_t levelWidth = m_Width;
		uint32_t levelHeight = m_Height;

		for (uint32_t level = 0; level < m_MipLevels; level++)
		{
			VkBufferImageCopy copyRegion{};
			copyRegion.bufferOffset = levelOffsets[level];
			copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
			copyRegion.imageExtent = { levelWidth, levelHeight, 1 };
			vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
			levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
		}

		VkImageMemoryBarrier2 toShaderRead{};
		toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toShaderRead.image = m_Image;
		toShaderRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_MipLevels, 0, 1 };

		VkDependencyInfo toShaderReadDep{};
		toShaderReadDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toShaderReadDep.imageMemoryBarrierCount = 1;
		toShaderReadDep.pImageMemoryBarriers = &toShaderRead;
		vkCmdPipelineBarrier2(cmd, &toShaderReadDep);

		device.EndSingleTimeCommands(cmd);

		// Inside an upload batch the recorded copies still reference this
		// staging - hand it to the batch (it dies once the GPU passes the
		// batch's submission). Outside a batch, destroy it now.
		if (!device.BatchRetireStaging(staging.GetHandle(), staging.GetAllocation()))
			staging.Shutdown();

		m_CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	void VulkanImage2D::UploadPrefixAndFillMips(const std::vector<std::vector<uint8_t>>& mipLevels, uint32_t firstMipCount)
	{
		HEARTH_ZONE_FUNCTION();

		VulkanDevice& device = *m_RhiDevice->GetVulkan();

		const uint32_t totalLevels = (uint32_t)mipLevels.size();
		// The finest level holding REAL pixels (the rest of the coarse
		// prefix is even smaller real data); every level BELOW this index
		// is filled by upscaling it (the fill-up - every level holds valid
		// data at every sample, so no LOD clamp is needed).
		const uint32_t finestResident = totalLevels - firstMipCount;

		// Staging sized for the prefix only (the fill-up needs no bytes -
		// it blits from the image itself).
		VkDeviceSize totalSize = 0;
		std::vector<VkDeviceSize> levelOffsets(firstMipCount);
		for (uint32_t i = 0; i < firstMipCount; i++)
		{
			levelOffsets[i] = totalSize;
			totalSize += mipLevels[finestResident + i].size();
		}

		VulkanBuffer staging;
		staging.Init(device, totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*hostVisible*/ true, "Streaming Prefix Upload Staging Buffer");
		for (uint32_t i = 0; i < firstMipCount; i++)
			staging.SetData(mipLevels[finestResident + i].data(), mipLevels[finestResident + i].size(), levelOffsets[i]);

		VkCommandBuffer cmd = device.BeginSingleTimeCommands();

		// Fresh image: whole chain UNDEFINED -> TRANSFER_DST. The prefix
		// levels get their copies below; the finer levels are the fill-up
		// blits' TRANSFER_DST destinations.
		{
			VkImageMemoryBarrier2 toDst{};
			toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
			toDst.srcAccessMask = VK_ACCESS_2_NONE;
			toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDst.image = m_Image;
			toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, totalLevels, 0, 1 };

			VkDependencyInfo toDstDep{};
			toDstDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			toDstDep.imageMemoryBarrierCount = 1;
			toDstDep.pImageMemoryBarriers = &toDst;
			vkCmdPipelineBarrier2(cmd, &toDstDep);
		}

		// Copy the coarse prefix (levels [finestResident, totalLevels)).
		uint32_t levelWidth = m_Width;
		uint32_t levelHeight = m_Height;
		for (uint32_t i = 0; i < finestResident; i++)
		{
			levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
			levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
		}
		for (uint32_t i = 0; i < firstMipCount; i++)
		{
			VkBufferImageCopy copyRegion{};
			copyRegion.bufferOffset = levelOffsets[i];
			copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, finestResident + i, 0, 1 };
			copyRegion.imageExtent = { levelWidth, levelHeight, 1 };
			vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

			levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
			levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
		}

		// The finest resident level becomes the fill-up blit source.
		{
			VkImageMemoryBarrier2 srcReady{};
			srcReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			srcReady.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			srcReady.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			srcReady.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			srcReady.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			srcReady.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			srcReady.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			srcReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			srcReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			srcReady.image = m_Image;
			srcReady.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, finestResident, 1, 0, 1 };

			VkDependencyInfo srcReadyDep{};
			srcReadyDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			srcReadyDep.imageMemoryBarrierCount = 1;
			srcReadyDep.pImageMemoryBarriers = &srcReady;
			vkCmdPipelineBarrier2(cmd, &srcReadyDep);
		}

		// Blit the finest resident level UP into every finer level (linear
		// filter - the fine levels hold upscaled coarse data, exactly the
		// "blurry" a mip-first stream wants). Level 0 is the largest.
		uint32_t srcWidth = m_Width;
		uint32_t srcHeight = m_Height;
		for (uint32_t i = 0; i < finestResident; i++)
		{
			srcWidth = srcWidth > 1 ? srcWidth / 2 : 1;
			srcHeight = srcHeight > 1 ? srcHeight / 2 : 1;
		}
		uint32_t dstWidth = m_Width;
		uint32_t dstHeight = m_Height;
		for (uint32_t level = 0; level < finestResident; level++)
		{
			VkImageBlit blit{};
			blit.srcOffsets[0] = { 0, 0, 0 };
			blit.srcOffsets[1] = { (int32_t)srcWidth, (int32_t)srcHeight, 1 };
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, finestResident, 0, 1 };
			blit.dstOffsets[0] = { 0, 0, 0 };
			blit.dstOffsets[1] = { (int32_t)dstWidth, (int32_t)dstHeight, 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };

			vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

			dstWidth = dstWidth > 1 ? dstWidth / 2 : 1;
			dstHeight = dstHeight > 1 ? dstHeight / 2 : 1;
		}

		// Final transitions back to SHADER_READ - three per-level groups
		// because the levels sit in two different old layouts (the same
		// multi-barrier tail UploadAndGenerateMips uses). The whole image
		// ends in SHADER_READ_ONLY_OPTIMAL, so the whole-image layout
		// tracker is correct at the frame boundary.
		std::vector<VkImageMemoryBarrier2> finalBarriers;

		// (a) the finer filled levels [0, finestResident): blit-written.
		if (finestResident > 0)
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = m_Image;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, finestResident, 0, 1 };
			finalBarriers.push_back(barrier);
		}

		// (b) the coarser prefix levels (finestResident, totalLevels): copy-written.
		if (firstMipCount > 1)
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = m_Image;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, finestResident + 1, firstMipCount - 1, 0, 1 };
			finalBarriers.push_back(barrier);
		}

		// (c) the blit source itself: read from, never written into.
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = m_Image;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, finestResident, 1, 0, 1 };
			finalBarriers.push_back(barrier);
		}

		VkDependencyInfo finalDep{};
		finalDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		finalDep.imageMemoryBarrierCount = (uint32_t)finalBarriers.size();
		finalDep.pImageMemoryBarriers = finalBarriers.data();
		vkCmdPipelineBarrier2(cmd, &finalDep);

		device.EndSingleTimeCommands(cmd);

		if (!device.BatchRetireStaging(staging.GetHandle(), staging.GetAllocation()))
			staging.Shutdown();

		m_CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	void VulkanImage2D::UploadMipLevel(uint32_t level, const std::vector<uint8_t>& pixels)
	{
		VulkanDevice& device = *m_RhiDevice->GetVulkan();

		HEARTH_ASSERT(level < m_MipLevels, "VulkanImage2D: UploadMipLevel level out of range!");

		uint32_t levelWidth = m_Width;
		uint32_t levelHeight = m_Height;
		for (uint32_t i = 0; i < level; i++)
		{
			levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
			levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
		}
		HEARTH_ASSERT(pixels.size() == (size_t)levelWidth * levelHeight * 4, "VulkanImage2D: UploadMipLevel byte size doesn't match its level dimensions!");

		VulkanBuffer staging;
		staging.Init(device, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*hostVisible*/ true, "Streaming Mip Upload Staging Buffer");
		staging.SetData(pixels.data(), pixels.size());

		VkCommandBuffer cmd = device.BeginSingleTimeCommands();

		// Whole-image transitions per batch (the RHI's layout tracker is
		// whole-image) - SHADER_READ -> TRANSFER_DST, one copy into the
		// level, whole image back to SHADER_READ. The queue-ordered
		// transition waits for the previous frame's sampling to finish
		// before the copy overwrites the level (one graphics queue, both
		// windows submit to it).
		const bool firstUpload = (m_CurrentLayout == VK_IMAGE_LAYOUT_UNDEFINED);

		{
			VkImageMemoryBarrier2 toDst{};
			toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			toDst.srcStageMask = firstUpload ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			toDst.srcAccessMask = firstUpload ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_READ_BIT;
			toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			toDst.oldLayout = m_CurrentLayout;
			toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDst.image = m_Image;
			toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_MipLevels, 0, 1 };

			VkDependencyInfo toDstDep{};
			toDstDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			toDstDep.imageMemoryBarrierCount = 1;
			toDstDep.pImageMemoryBarriers = &toDst;
			vkCmdPipelineBarrier2(cmd, &toDstDep);
		}

		VkBufferImageCopy copyRegion{};
		copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
		copyRegion.imageExtent = { levelWidth, levelHeight, 1 };
		vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

		{
			VkImageMemoryBarrier2 toShaderRead{};
			toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toShaderRead.image = m_Image;
			toShaderRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_MipLevels, 0, 1 };

			VkDependencyInfo toShaderReadDep{};
			toShaderReadDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			toShaderReadDep.imageMemoryBarrierCount = 1;
			toShaderReadDep.pImageMemoryBarriers = &toShaderRead;
			vkCmdPipelineBarrier2(cmd, &toShaderReadDep);
		}

		device.EndSingleTimeCommands(cmd);

		if (!device.BatchRetireStaging(staging.GetHandle(), staging.GetAllocation()))
			staging.Shutdown();

		m_CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	void VulkanImage2D::SetData(const void* data, uint32_t size)
	{
		HEARTH_ZONE_FUNCTION();

		uint32_t expectedSize = m_Width * m_Height * 4;
		HEARTH_ASSERT(size == expectedSize, "Data Must Be Entire Texture!");

		UploadAndGenerateMips(data);
	}


}
