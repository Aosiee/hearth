#pragma once

#include "hearth/rhi/vulkan/VulkanCommon.h"

#include <cstdint>
#include <span>
#include <vector>

namespace hearth::rhi {

	class VulkanRhiDevice;

	// A sampled 2D image in the global bindless set: always RGBA8 (the one
	// four-channel format every conformant driver supports; three-channel
	// formats are optional), with a full mip chain built by blits, or a
	// baked chain uploaded level by level, or a render target of any format.
	// Its destruction is deferred through the device's retire queue, so an
	// in-flight frame may still sample it.
	class VulkanImage2D
	{
	public:
		// Zero-filled (a texture filled later through SetData).
		VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, bool srgb = false);
		// From its RGBA8 pixels (width * height * 4 bytes), the mip chain
		// built from them.
		VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, std::span<const uint8_t> rgba8, bool srgb = false);

		// A graph-transient RENDER TARGET - the image is created with the
		// requested format + colour/depth-attachment usage (+ SAMPLED, so
		// the lighting/post passes read it back), never uploaded. The
		// bindless entry is written for the format's own view (a nearest
		// sampler for depth targets - depth must never be linearly
		// filtered).
		VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, VkFormat format, bool depthTarget);
		// Baked mip chain upload - every level
		// already has correct pixel data; this only issues one
		// buffer-to-image copy per level, no vkCmdBlitImage chain at all.
		// A firstMipCount of 0 (or >= the chain length) uploads the whole
		// chain; otherwise the coarse prefix uploads and every finer level
		// is filled by upscaling the finest resident level.
		VulkanImage2D(VulkanRhiDevice& rhiDevice, uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>>& mipLevels, uint32_t firstMipCount = 0);
		~VulkanImage2D();
		VulkanImage2D(const VulkanImage2D&) = delete;
		VulkanImage2D& operator=(const VulkanImage2D&) = delete;

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		// The whole image's RGBA8 pixels; the mip chain is rebuilt.
		void SetData(const void* data, uint32_t size);

		void UpdateBakedMips(uint32_t width, uint32_t height, const std::vector<std::vector<uint8_t>>& mipLevels);

		// Single-level in-place upload (the streaming follow-up batch).
		// Whole-image SHADER_READ -> TRANSFER_DST, one copy, whole image
		// back - the whole-image layout tracker stays true at every frame
		// boundary.
		void UploadMipLevel(uint32_t level, const std::vector<uint8_t>& pixels);

		VkImageView GetImageView() const { return m_ImageView; }
		VkImage GetImage() const { return m_Image; }
		VkSampler GetSampler() const { return m_Sampler; }
		VkImageLayout GetCurrentLayout() const { return m_CurrentLayout; }

		// The image's slot in the global bindless set (assigned at
		// creation, written immediately, recycled through the retire queue).
		uint32_t GetBindlessIndex() const { return m_BindlessIndex; }
		// The `_SRGB` view's own bindless slot (equal to GetBindlessIndex
		// for linear textures - one entry).
		uint32_t GetBindlessIndexSrgb() const { return m_BindlessIndexSrgb; }
		bool IsSrgb() const { return m_Srgb; }

	private:
		// mipCount of 0 means "compute the full chain from width/height"
		// (the non-baked constructors); the baked-mip constructor passes
		// its own mip count explicitly, since that has to match however many
		// levels the cooker actually baked, not whatever a fresh log2
		// computation would produce.
		void CreateImage(uint32_t width, uint32_t height, uint32_t mipCount = 0);
		void CreateImageRenderTarget(uint32_t width, uint32_t height, VkFormat format, bool depthTarget);
		void CreateSampler();
		void AcquireBindlessSlot();
		void UploadAndGenerateMips(const void* pixels);
		// The streaming first promotion - copy the coarse prefix and blit
		// the finest resident level up into every finer level.
		void UploadPrefixAndFillMips(const std::vector<std::vector<uint8_t>>& mipLevels, uint32_t firstMipCount);

	private:
		VulkanRhiDevice* m_RhiDevice = nullptr;
		uint32_t m_Width = 0, m_Height = 0;
		uint32_t m_MipLevels = 1;
		uint32_t m_BindlessIndex = 0;
		uint32_t m_BindlessIndexSrgb = 0;
		bool m_Srgb = false;

		VkImage m_Image = VK_NULL_HANDLE;
		VmaAllocation m_Allocation = VK_NULL_HANDLE;
		VkImageView m_ImageView = VK_NULL_HANDLE;
		VkImageView m_ImageViewSrgb = VK_NULL_HANDLE;
		VkSampler m_Sampler = VK_NULL_HANDLE;

		VkFormat m_Format = VK_FORMAT_R8G8B8A8_UNORM;
		VkImageLayout m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

} // namespace hearth::rhi
