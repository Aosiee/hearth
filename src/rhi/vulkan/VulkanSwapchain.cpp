#include "tepch.h"
#include "Platform/Vulkan/VulkanSwapchain.h"

#include "Platform/Vulkan/RHI/VulkanRhiDevice.h"

#include <GLFW/glfw3.h>

namespace TinyEngine {

	void VulkanSwapchain::Init(VulkanDevice& device, GLFWwindow* window)
	{
		TE_PROFILE_FUNCTION();

		m_Device = &device;
		m_Window = window;

		CreateSurface(window);

		// F1: the per-swapchain transition pool (created on the game thread
		// during Init, before the submit thread exists; used only by the
		// submit thread's depth-transition during a resize).
		CreateTransitionCommandPool();

		int width, height;
		glfwGetFramebufferSize(window, &width, &height);
		CreateSwapchain((uint32_t)width, (uint32_t)height);
		CreateImageViews();
		CreateSyncObjects();
	}

	void VulkanSwapchain::CreateTransitionCommandPool()
	{
		VkCommandPoolCreateInfo poolCreateInfo{};
		poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		poolCreateInfo.queueFamilyIndex = m_Device->GetGraphicsQueueFamily();
		VK_CHECK(vkCreateCommandPool(m_Device->GetDevice(), &poolCreateInfo, nullptr, &m_TransitionCommandPool));
		m_Device->SetObjectName(VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)m_TransitionCommandPool, "Swapchain Transition Command Pool");
	}

	void VulkanSwapchain::DestroyTransitionCommandPool()
	{
		if (m_TransitionCommandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(m_Device->GetDevice(), m_TransitionCommandPool, nullptr);
			m_TransitionCommandPool = VK_NULL_HANDLE;
		}
	}

	VulkanSwapchain::~VulkanSwapchain()
	{
		Shutdown();
	}

	void VulkanSwapchain::Shutdown()
	{
		TE_PROFILE_FUNCTION();

		if (m_Device == nullptr)
			return; // idempotent: the destructor runs this after an explicit Shutdown

		VkDevice device = m_Device->GetDevice();
		// F2: the device-wait touches every queue - scope it under the queue
		// mutex (other windows' submit threads may be live; the lock covers
		// ONLY the wait, never a re-entrant upload).
		{
			std::lock_guard queueLock(m_Device->GetQueueMutex());
			vkDeviceWaitIdle(device);
		}

		DestroyTransitionCommandPool();
		DestroySwapchainResources();

		for (VkSemaphore& semaphore : m_ImageAvailableSemaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(device, semaphore, nullptr);
			semaphore = VK_NULL_HANDLE;
		}

		if (m_Surface != VK_NULL_HANDLE)
			vkDestroySurfaceKHR(m_Device->GetInstance(), m_Surface, nullptr);
		m_Surface = VK_NULL_HANDLE;

		m_Device = nullptr;
	}

	void VulkanSwapchain::CreateSurface(GLFWwindow* window)
	{
		VK_CHECK(glfwCreateWindowSurface(m_Device->GetInstance(), window, nullptr, &m_Surface));

		VkBool32 presentSupported = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(m_Device->GetPhysicalDevice(), m_Device->GetGraphicsQueueFamily(), m_Surface, &presentSupported);
		TE_CORE_ASSERT(presentSupported, "The graphics queue family chosen at device creation cannot present to this window's surface!");
	}

	VkSurfaceFormatKHR VulkanSwapchain::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const
	{
		// Deliberately UNORM, not SRGB, even though VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
		// (the colour space, a separate thing from the format) is still what is
		// wanted. An _SRGB *format* makes the GPU automatically apply a linear ->
		// sRGB encode when the fragment shader writes to this attachment, which
		// is only correct if the shader is actually working in linear colour
		// (i.e. textures were sampled through an _SRGB format that decoded them
		// first). VulkanTexture2D samples as plain UNORM (no decode, see its
		// header), so the shader is really just passing already-sRGB-encoded
		// bytes through unmodified — writing that through an _SRGB attachment
		// double-encodes it, which reads as washed out, lower contrast colour.
		// GL never does any gamma correction anywhere in this renderer (no
		// GL_FRAMEBUFFER_SRGB, no sRGB texture formats), so matching that
		// "untouched bytes all the way through" pipeline means the swapchain
		// shouldn't apply one either.
		for (const VkSurfaceFormatKHR& format : available)
		{
			if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				return format;
		}

		return available[0];
	}

	VkPresentModeKHR VulkanSwapchain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& available) const
	{
		if (!m_VsyncEnabled)
		{
			// True vsync off: present as soon as a frame is ready, uncapped,
			// tearing allowed. Not every platform actually exposes this
			// (Wayland compositors commonly don't, Hyprland included on some
			// driver/compositor combinations), so fall through to the
			// vsync-on search below rather than silently staying on
			// whatever mode was already active if it isn't there.
			for (VkPresentModeKHR mode : available)
			{
				if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
					return mode;
			}
		}

		for (VkPresentModeKHR mode : available)
		{
			// Mailbox gives the lowest latency without tearing when it's there,
			// but FIFO is the only mode the spec actually guarantees, so it's the
			// safe fallback everywhere else.
			if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
				return mode;
		}

		return VK_PRESENT_MODE_FIFO_KHR;
	}

	// The game thread (the r.vsync cvar observer) records the request AND
	// applies it (from VulkanContext::SwapBuffers, under the submit-idle
	// park) - the swapchain rebuild must never run on the submit thread
	// while the game thread owns the RHI handle tables.
	void VulkanSwapchain::SetVsync(bool enabled)
	{
		m_PendingVsync.store(enabled ? 1u : 0u, std::memory_order_relaxed);
	}

	void VulkanSwapchain::ApplyPendingVsync()
	{
		uint32_t pending = m_PendingVsync.load(std::memory_order_relaxed);
		if (pending > 1)
			return; // no pending request
		bool enabled = pending != 0;

		// Present mode is only chosen at swapchain build time (CreateSwapchain,
		// called from here), there's no way to change an already-built
		// swapchain's mode in place - has to rebuild. A no-op if nothing has
		// been created yet; Init() picks up m_VsyncEnabled's current value on
		// its own first build regardless.
		if (m_VsyncEnabled != enabled && m_RequestedWidth != 0 && m_RequestedHeight != 0)
		{
			m_VsyncEnabled = enabled;
			RecreateFromSurfaceCapabilities();
		}
		else
		{
			m_VsyncEnabled = enabled;
		}
		m_PendingVsync.store(2u, std::memory_order_relaxed); // consumed
	}

	VkExtent2D VulkanSwapchain::ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height) const
	{
		if (capabilities.currentExtent.width != UINT32_MAX)
			return capabilities.currentExtent;

		VkExtent2D extent{ width, height };
		extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
		extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
		return extent;
	}

	void VulkanSwapchain::CreateSwapchain(uint32_t width, uint32_t height)
	{
		m_RequestedWidth = width;
		m_RequestedHeight = height;

		// A zero sized surface means the window is minimized, there's nothing
		// sensible to build a swapchain against yet. The caller (VulkanContext)
		// is expected to skip drawing entirely in that case rather than call in
		// here, but bail out safely regardless.
		if (width == 0 || height == 0)
			return;

		VkPhysicalDevice physicalDevice = m_Device->GetPhysicalDevice();

		VkSurfaceCapabilitiesKHR capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_Surface, &capabilities);

		uint32_t formatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_Surface, &formatCount, nullptr);
		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_Surface, &formatCount, formats.data());

		uint32_t presentModeCount = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_Surface, &presentModeCount, nullptr);
		std::vector<VkPresentModeKHR> presentModes(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_Surface, &presentModeCount, presentModes.data());

		VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);
		VkPresentModeKHR presentMode = ChoosePresentMode(presentModes);
		m_PresentMode = presentMode;
		VkExtent2D extent = ChooseExtent(capabilities, width, height);

		uint32_t imageCount = capabilities.minImageCount + 1;
		if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
			imageCount = capabilities.maxImageCount;

		VkSwapchainKHR oldSwapchain = m_Swapchain;

		VkSwapchainCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = m_Surface;
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = extent;
		createInfo.imageArrayLayers = 1;
		createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // one queue family owns both graphics and present
		createInfo.preTransform = capabilities.currentTransform;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;
		createInfo.oldSwapchain = oldSwapchain;

		VK_CHECK(vkCreateSwapchainKHR(m_Device->GetDevice(), &createInfo, nullptr, &m_Swapchain));

		if (oldSwapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(m_Device->GetDevice(), oldSwapchain, nullptr);

		m_ImageFormat = surfaceFormat.format;
		m_Extent = extent;

		uint32_t actualImageCount = 0;
		vkGetSwapchainImagesKHR(m_Device->GetDevice(), m_Swapchain, &actualImageCount, nullptr);
		m_Images.resize(actualImageCount);
		vkGetSwapchainImagesKHR(m_Device->GetDevice(), m_Swapchain, &actualImageCount, m_Images.data());

		CreateDepthResources();

		TE_CORE_INFO(LogVulkan, "Vulkan Swapchain Created: {0}x{1}, {2} images, present mode {3}", extent.width, extent.height, actualImageCount, (int)presentMode);
	}

	void VulkanSwapchain::CreateDepthResources()
	{
		VkImageCreateInfo imageCreateInfo{};
		imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.format = GetDepthFormat();
		imageCreateInfo.extent = { m_Extent.width, m_Extent.height, 1 };
		imageCreateInfo.mipLevels = 1;
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
			// SAMPLED so the deferred lighting pass reads the depth back
			// (the framebuffer's depth-wrap precedent).
			| VK_IMAGE_USAGE_SAMPLED_BIT;
		imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VmaAllocationCreateInfo allocCreateInfo{};
		allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VK_CHECK(vmaCreateImage(m_Device->GetAllocator(), &imageCreateInfo, &allocCreateInfo, &m_DepthImage, &m_DepthAllocation, nullptr));
		m_Device->SetObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)m_DepthImage, "Swapchain Depth Image");

		VkImageViewCreateInfo viewCreateInfo{};
		viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewCreateInfo.image = m_DepthImage;
		viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewCreateInfo.format = GetDepthFormat();
		viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewCreateInfo.subresourceRange.baseMipLevel = 0;
		viewCreateInfo.subresourceRange.levelCount = 1;
		viewCreateInfo.subresourceRange.baseArrayLayer = 0;
		viewCreateInfo.subresourceRange.layerCount = 1;

		VK_CHECK(vkCreateImageView(m_Device->GetDevice(), &viewCreateInfo, nullptr, &m_DepthImageView));
		m_Device->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)m_DepthImageView, "Swapchain Depth Image View");

		// The sample-read wrap - a non-comparison NEAREST sampler (depth
		// must never be linearly filtered) + the bindless slot the lighting
		// pass reads. The descriptor's layout is the depth READ layout (the
		// RHI's canonical DepthRead), so the lighting pass's graph-derived
		// barrier must land there.
		VkSamplerCreateInfo depthSamplerCreateInfo{};
		depthSamplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		depthSamplerCreateInfo.magFilter = VK_FILTER_NEAREST;
		depthSamplerCreateInfo.minFilter = VK_FILTER_NEAREST;
		depthSamplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		depthSamplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		depthSamplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		depthSamplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		depthSamplerCreateInfo.minLod = 0.0f;
		depthSamplerCreateInfo.maxLod = 0.0f;
		VK_CHECK(vkCreateSampler(m_Device->GetDevice(), &depthSamplerCreateInfo, nullptr, &m_DepthSampler));

		rhi::VulkanRhiDevice& rhiDevice = GetVulkanRhiDevice();
		m_DepthBindlessIndex = rhiDevice.AllocateBindlessTextureIndex();
		// SHADER_READ_ONLY_OPTIMAL, matching the framebuffer's own depth
		// descriptor + the graph's ShaderRead state (the one sampled layout
		// the whole tree uses for depth-only images).
		rhiDevice.WriteTextureDescriptor(m_DepthBindlessIndex, m_DepthImageView, m_DepthSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		m_DepthGeneration++;

		// One-time UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL transition, right here
		// at creation rather than every frame in VulkanContext::RecordClearPass.
		// This image is never read anywhere else (no sampling, no present), and
		// it's cleared in full every frame, so once it's in this layout there's
		// never a reason to leave it - see VulkanSwapchain.h's GetDepthImageView
		// comment. F1: this runs on the SUBMIT thread during a resize, so it
		// uses the swapchain's OWN transient pool, never the device-wide one
		// the game thread's uploads share.
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = m_TransitionCommandPool;
		allocInfo.commandBufferCount = 1;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VK_CHECK(vkAllocateCommandBuffers(m_Device->GetDevice(), &allocInfo, &commandBuffer));

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

		VkImageMemoryBarrier2 toDepthAttachment{};
		toDepthAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		toDepthAttachment.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		toDepthAttachment.srcAccessMask = VK_ACCESS_2_NONE;
		toDepthAttachment.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		toDepthAttachment.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		toDepthAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDepthAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
		toDepthAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepthAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepthAttachment.image = m_DepthImage;
		toDepthAttachment.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

		VkDependencyInfo toDepthAttachmentDependency{};
		toDepthAttachmentDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		toDepthAttachmentDependency.imageMemoryBarrierCount = 1;
		toDepthAttachmentDependency.pImageMemoryBarriers = &toDepthAttachment;
		vkCmdPipelineBarrier2(commandBuffer, &toDepthAttachmentDependency);

		VK_CHECK(vkEndCommandBuffer(commandBuffer));

		uint64_t signalValue = 0;
		VkTimelineSemaphoreSubmitInfo timelineInfo{};
		timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timelineInfo.signalSemaphoreValueCount = 1;
		timelineInfo.pSignalSemaphoreValues = &signalValue;

		VkSemaphore timelineSemaphore = m_Device->GetTimelineSemaphore();
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.pNext = &timelineInfo;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &timelineSemaphore;

		// Scoped queue lock (submit) - the transition must land before the
		// image is used, and this runs on the submit thread while other
		// windows' frames submit on the same queue. The timeline VALUE is
		// allocated inside the lock; the wait is on the specific signal,
		// not a drain of the whole queue.
		{
			std::lock_guard queueLock(m_Device->GetQueueMutex());
			signalValue = m_Device->AllocateTimelineValue();
			VK_CHECK(vkQueueSubmit(m_Device->GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
		}
		m_Device->WaitForTimelineValue(signalValue);

		vkFreeCommandBuffers(m_Device->GetDevice(), m_TransitionCommandPool, 1, &commandBuffer);
	}

	void VulkanSwapchain::CreateImageViews()
	{
		m_ImageViews.resize(m_Images.size());

		for (size_t i = 0; i < m_Images.size(); i++)
		{
			VkImageViewCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			createInfo.image = m_Images[i];
			createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			createInfo.format = m_ImageFormat;
			createInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
			createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			createInfo.subresourceRange.baseMipLevel = 0;
			createInfo.subresourceRange.levelCount = 1;
			createInfo.subresourceRange.baseArrayLayer = 0;
			createInfo.subresourceRange.layerCount = 1;

			VK_CHECK(vkCreateImageView(m_Device->GetDevice(), &createInfo, nullptr, &m_ImageViews[i]));
			m_Device->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)m_ImageViews[i], "Swapchain Image View " + std::to_string(i));
		}

		// P4-B: adopt each image into the RHI as a barrier-target handle so
		// the RenderGraph can Import it (no view/bindless slot - the graph
		// only barriers it).
		{
			m_ImageHandles.clear();
			rhi::Device& rhiDevice = GetVulkanRhiDevice();
			for (size_t i = 0; i < m_Images.size(); i++)
				m_ImageHandles.push_back(rhiDevice.AdoptExternalImage((void*)m_Images[i]));
		}

		// Fresh set of images means none of them belong to an in-flight frame yet.
		m_ImagesInFlight.assign(m_Images.size(), 0);

		// Render finished semaphores are per image (see header comment), so they
		// need to be (re)created whenever the image count changes.
		for (VkSemaphore semaphore : m_RenderFinishedSemaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(m_Device->GetDevice(), semaphore, nullptr);
		}

		m_RenderFinishedSemaphores.resize(m_Images.size());

		VkSemaphoreCreateInfo semaphoreCreateInfo{};
		semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		for (size_t i = 0; i < m_RenderFinishedSemaphores.size(); i++)
		{
			VK_CHECK(vkCreateSemaphore(m_Device->GetDevice(), &semaphoreCreateInfo, nullptr, &m_RenderFinishedSemaphores[i]));
			m_Device->SetObjectName(VK_OBJECT_TYPE_SEMAPHORE, (uint64_t)m_RenderFinishedSemaphores[i], "Render Finished Semaphore (image " + std::to_string(i) + ")");
		}
	}

	void VulkanSwapchain::CreateSyncObjects()
	{
		VkSemaphoreCreateInfo semaphoreCreateInfo{};
		semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
		{
			VK_CHECK(vkCreateSemaphore(m_Device->GetDevice(), &semaphoreCreateInfo, nullptr, &m_ImageAvailableSemaphores[i]));
			m_Device->SetObjectName(VK_OBJECT_TYPE_SEMAPHORE, (uint64_t)m_ImageAvailableSemaphores[i], "Image Available Semaphore (frame " + std::to_string(i) + ")");
			// P-A2: no fences - the frame slot's first acquire waits on
			// timeline value 0 (an immediate no-op).
			m_FrameLastValues[i].store(0);
		}
	}

	void VulkanSwapchain::DestroySwapchainResources()
	{
		VkDevice device = m_Device->GetDevice();

		for (VkSemaphore semaphore : m_RenderFinishedSemaphores)
		{
			if (semaphore != VK_NULL_HANDLE)
				vkDestroySemaphore(device, semaphore, nullptr);
		}
		m_RenderFinishedSemaphores.clear();

		for (VkImageView view : m_ImageViews)
			vkDestroyImageView(device, view, nullptr);
		m_ImageViews.clear();
		m_Images.clear();

		// P4-B: release the adopted image handles (nothing is destroyed -
		// the images themselves die with the swapchain above). Requires the
		// RHI device to still be alive - the window (and this swapchain)
		// dies before Engine::Shutdown tears the device down.
		{
			rhi::Device& rhiDevice = GetVulkanRhiDevice();
			for (rhi::TextureHandle handle : m_ImageHandles)
			{
				if (rhi::IsValid(handle))
					rhiDevice.DestroyTexture(handle);
			}
			m_ImageHandles.clear();
		}

		// The depth's sample-read wrap retires through the queue (the
		// bindless slot's tombstone + the in-flight frames still sampling it)
		// instead of destroying synchronously.
		if (m_DepthImage != VK_NULL_HANDLE)
		{
			rhi::VulkanRhiDevice& rhiDevice = GetVulkanRhiDevice();
			// One extra submission before the destroy - the frames recorded
			// against this depth hold command buffers that reset at the
			// next frame's pool reset; destroying at the GPU-completion
			// stamp landed one generation early (the startup
			// double-recreate's vkDestroyImage-image-01000). The depth's
			// destroy holds several submissions: the recorded
			// command-buffer references live until the per-slot pools reset
			// (two full slot cycles), and the startup/rapid-resize
			// double-recreate freed it one generation early at the +1/+2
			// (the vkDestroyImage-image-01000).
			rhiDevice.QueueTextureForDestruction(m_DepthImage, m_DepthAllocation, m_DepthImageView, m_DepthSampler, m_DepthBindlessIndex);
		}
		m_DepthSampler = VK_NULL_HANDLE;
		m_DepthBindlessIndex = 0;
		m_DepthImageView = VK_NULL_HANDLE;
		m_DepthImage = VK_NULL_HANDLE;
		m_DepthAllocation = VK_NULL_HANDLE;

		if (m_Swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
			m_Swapchain = VK_NULL_HANDLE;
		}
	}

	// T-F-C: the out-of-date rebuild sizes from the surface capabilities
	// (the submit thread owns the surface - no GLFW calls off the main
	// thread), falling back to the game-thread-published framebuffer size
	// when the compositor reports an undefined currentExtent.
	void VulkanSwapchain::RecreateFromSurfaceCapabilities()
	{
		VkSurfaceCapabilitiesKHR capabilities{};
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Device->GetPhysicalDevice(), m_Surface, &capabilities));

		if (capabilities.currentExtent.width != 0xFFFFFFFFu && capabilities.currentExtent.width != 0)
			Recreate(capabilities.currentExtent.width, capabilities.currentExtent.height);
		else
			Recreate(m_PublishedWidth.load(std::memory_order_relaxed), m_PublishedHeight.load(std::memory_order_relaxed));
	}

	void VulkanSwapchain::Recreate(uint32_t width, uint32_t height)
	{
		TE_PROFILE_FUNCTION();

		if (width == 0 || height == 0)
			return;

		// T-F-C: the queue is externally synchronized - the lock guards
		// only the device-wait itself: the rebuild below runs uploads
		// (CreateDepthResources' one-time transition) that take the same
		// mutex per-op, so holding it across the rebuild would self-deadlock.
		{
			std::lock_guard queueLock(m_Device->GetQueueMutex());
			vkDeviceWaitIdle(m_Device->GetDevice());
		}

		DestroySwapchainResources();
		CreateSwapchain(width, height);
		CreateImageViews();
	}

	VulkanSwapchain::AcquireResult VulkanSwapchain::AcquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex)
	{
		TE_PROFILE_FUNCTION();

		VkDevice device = m_Device->GetDevice();

		// P-A2: the frame slot's previous submission must have completed
		// before its command pool/buffer is reused (the timeline replaces
		// the old per-frame fence).
		{
			TE_PROFILE_SCOPE("Acquire: frame-slot timeline wait");
			m_Device->WaitForTimelineValue(m_FrameLastValues[frameIndex].load());
		}

		// T-F-C: bounded wait - the acquire runs on the dedicated submit
		// thread and must not wedge the shutdown join; the timeout lets a
		// shutdown request break the loop.
		VkResult result = VK_TIMEOUT;
		while (result == VK_TIMEOUT || result == VK_NOT_READY)
		{
			{
				TE_PROFILE_SCOPE("Acquire: vkAcquireNextImageKHR");
				result = vkAcquireNextImageKHR(device, m_Swapchain, kAcquireTimeoutNs, m_ImageAvailableSemaphores[frameIndex], VK_NULL_HANDLE, &outImageIndex);
			}
			if ((result == VK_TIMEOUT || result == VK_NOT_READY) && IsShutdownRequested())
				return AcquireResult::ShuttingDown;
		}

		if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			// Raise the rebuild request - the game thread performs the
			// rebuild (under the park) at its next SwapBuffers.
			RequestRebuild();
			return AcquireResult::OutOfDate;
		}

		if (result == VK_ERROR_SURFACE_LOST_KHR)
		{
			TE_CORE_WARN(LogVulkan, "vkAcquireNextImageKHR lost the surface - skipping this frame.");
			return AcquireResult::ShuttingDown;
		}

		TE_CORE_ASSERT(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "vkAcquireNextImageKHR failed unexpectedly!");

		// If a previous frame in flight is still using this exact image (can
		// happen once the swapchain has more images than frames in flight), wait
		// for it before writing over resources it might still be reading from.
		if (m_ImagesInFlight[outImageIndex] != 0)
		{
			TE_PROFILE_SCOPE("Acquire: image-reuse timeline wait");
			m_Device->WaitForTimelineValue(m_ImagesInFlight[outImageIndex]);
		}

		return AcquireResult::Success;
	}

	void VulkanSwapchain::SubmitAndPresent(uint32_t frameIndex, uint32_t imageIndex, VkCommandBuffer commandBuffer)
	{
		SubmitAndPresent(frameIndex, imageIndex, std::vector<VkCommandBuffer>{ commandBuffer });
	}

	void VulkanSwapchain::SubmitAndPresent(uint32_t frameIndex, uint32_t imageIndex, const std::vector<VkCommandBuffer>& commandBuffers)
	{
		TE_PROFILE_FUNCTION();

		VkSemaphore waitSemaphores[] = { m_ImageAvailableSemaphores[frameIndex] };
		VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
		VkSemaphore signalSemaphores[] = { m_RenderFinishedSemaphores[imageIndex] };

		// One signal value per signalled semaphore; the binary render-finished
		// entry is ignored (binary semaphores have no value), the timeline one
		// carries the fresh value (VUID-VkSubmitInfo-pNext-03241).
		uint64_t timelineValue = 0;
		uint64_t signalValues[] = { 0, 0 };

		VkTimelineSemaphoreSubmitInfo timelineSubmit{};
		timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timelineSubmit.signalSemaphoreValueCount = 2;
		timelineSubmit.pSignalSemaphoreValues = signalValues;
		VkSemaphore timelineSemaphore = m_Device->GetTimelineSemaphore();

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pNext = &timelineSubmit;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
		submitInfo.commandBufferCount = (uint32_t)commandBuffers.size();
		submitInfo.pCommandBuffers = commandBuffers.empty() ? nullptr : commandBuffers.data();
		submitInfo.signalSemaphoreCount = 2;
		VkSemaphore bothSignals[] = { signalSemaphores[0], timelineSemaphore };
		submitInfo.pSignalSemaphores = bothSignals;

		// T-F-C: the queue is externally synchronized - the frame submit
		// and the game thread's upload submits serialize on it. The timeline
		// VALUE is allocated INSIDE the same lock as the submit: with
		// multiple windows (or the asset upload batch) allocating values
		// concurrently, an out-of-order execution of two submissions would
		// otherwise signal a value <= the current one
		// (VUID-VkSubmitInfo-pSignalSemaphores-03242).
		{
			std::lock_guard queueLock(m_Device->GetQueueMutex());
			timelineValue = m_Device->AllocateTimelineValue();
			signalValues[1] = timelineValue;
			VK_CHECK(vkQueueSubmit(m_Device->GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
		}

		// P-A3 note: the retire drain now runs on the game thread after the
		// frame-complete wait (VulkanContext:: SwapBuffers) - the retire
		// queue + the VMA allocator never cross threads.
		m_ImagesInFlight[imageIndex] = timelineValue;
		m_FrameLastValues[frameIndex].store(timelineValue);

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &m_Swapchain;
		presentInfo.pImageIndices = &imageIndex;

		VkResult result;
		{
			std::lock_guard queueLock(m_Device->GetQueueMutex());
			result = vkQueuePresentKHR(m_Device->GetGraphicsQueue(), &presentInfo);
		}

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
			RequestRebuild();
		else
			VK_CHECK(result);
	}

}
