#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "TinyEngine/RHI/RHI.h"

#include <atomic>

struct GLFWwindow;

namespace TinyEngine {

	// Owns the window's VkSurfaceKHR, its VkSwapchainKHR, and the sync objects
	// needed to safely acquire and present images. One of these exists per
	// window, borrowing a VulkanDevice that may be shared by several windows
	// (relevant when the UI layer gives every detached panel its own
	// window and swapchain).
	class VulkanSwapchain
	{
	public:
		void Init(VulkanDevice& device, GLFWwindow* window);
		void Shutdown();
		// Self-cleaning: the RHI owns swapchains in a handle table whose
		// clear path destroys objects directly, so the destructor must
		// release the Vulkan objects (Shutdown is idempotent).
		~VulkanSwapchain();

		// Called when the window resizes. Cheap to call even if the size hasn't
		// actually changed, callers don't need to track that themselves.
		void Recreate(uint32_t width, uint32_t height);

		// T-F-C: the out-of-date rebuild path (submit thread) sizes from the
		// surface capabilities, falling back to the published framebuffer size.
		void RecreateFromSurfaceCapabilities();

		// Rebuilds the swapchain (same as Recreate, at its current size) so
		// ChoosePresentMode picks up the new preference immediately rather
		// than waiting for some unrelated resize to happen to notice it.
		// A no-op if enabled already matches what's already selected and a
		// swapchain already exists, so toggling this every frame by mistake
		// wouldn't be a problem.
		void SetVsync(bool enabled);

		// The submit thread never rebuilds - the out-of-date paths raise a
		// rebuild REQUEST; the game thread consumes it (under the
		// submit-idle park) at its next SwapBuffers, where the rebuild's RHI
		// handle-table ops cannot race the game thread's own texture ops.
		void RequestRebuild() { m_PendingRebuild.store(1u, std::memory_order_relaxed); }
		bool ConsumeRebuildRequest() { return m_PendingRebuild.exchange(0u, std::memory_order_relaxed) != 0; }
		// True while a SetVsync request is unconsumed (the apply may
		// rebuild, so the game thread parks before it runs).
		bool HasPendingVsync() const { return m_PendingVsync.load(std::memory_order_relaxed) < 2u; }

		// Waits on the current frame's fence, then acquires the next swapchain
		// image. OutOfDate means the swapchain came back out of date and had to
		// be rebuilt, in which case the caller should skip drawing this frame
		// rather than push a stale image through the pipeline. ShuttingDown
		// means the surface stopped handing out images after a shutdown
		// request (T-F-C: the acquire runs on the dedicated submit thread and
		// must not wedge the shutdown join).
		enum class AcquireResult : uint8_t
		{
			Success,
			OutOfDate,
			ShuttingDown,
		};
		AcquireResult AcquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex);

		// T-F-C: the bounded acquire retries on this timeout so a shutdown
		// request can break the loop (100ms).
		static constexpr uint64_t kAcquireTimeoutNs = 100'000'000;

		// T-F-C: the game thread publishes the window's framebuffer size
		// every frame; the submit thread's out-of-date rebuilds size from
		// the surface capabilities (falling back to this) instead of
		// calling GLFW off the main thread.
		void PublishFramebufferSize(uint32_t width, uint32_t height)
		{
			m_PublishedWidth.store(width, std::memory_order_relaxed);
			m_PublishedHeight.store(height, std::memory_order_relaxed);
		}
		VkExtent2D GetPublishedFramebufferSize() const
		{
			return { m_PublishedWidth.load(std::memory_order_relaxed), m_PublishedHeight.load(std::memory_order_relaxed) };
		}
		// T-F-C: the submit thread applies a pending vsync change at its
		// frame start (the swapchain rebuild must not run on the game
		// thread once the submit thread owns acquire/present).
		void ApplyPendingVsync();
		void RequestShutdown() { m_ShutdownRequested.store(true, std::memory_order_relaxed); }
		bool IsShutdownRequested() const { return m_ShutdownRequested.load(std::memory_order_relaxed); }

		// Submits an already recorded command buffer for the acquired image and
		// presents it. Handles the out of date / suboptimal case by recreating
		// the swapchain, in which case this frame is simply not shown.
		void SubmitAndPresent(uint32_t frameIndex, uint32_t imageIndex, VkCommandBuffer commandBuffer);
		// T-F-B.2: the per-pass command lists, submitted in order by ONE
		// queue submission.
		void SubmitAndPresent(uint32_t frameIndex, uint32_t imageIndex, const std::vector<VkCommandBuffer>& commandBuffers);

		VkFormat GetImageFormat() const { return m_ImageFormat; }
		VkExtent2D GetExtent() const { return m_Extent; }
		VkPresentModeKHR GetPresentMode() const { return m_PresentMode; }
		VkImage GetImage(uint32_t index) const { return m_Images[index]; }
		VkImageView GetImageView(uint32_t index) const { return m_ImageViews[index]; }
		uint32_t GetImageCount() const { return (uint32_t)m_Images.size(); }

		// P4-B: each swapchain image is adopted into the RHI as a
		// barrier-target handle (the RenderGraph imports it per frame).
		TinyEngine::rhi::TextureHandle GetImageHandle(uint32_t index) const
		{
			TE_CORE_ASSERT(index < m_ImageHandles.size(), "swapchain image index out of range");
			return m_ImageHandles[index];
		}

		// One depth image total, not one per swapchain image - unlike colour,
		// nothing ever needs to read back a previous frame's depth contents,
		// every frame's depth pass starts from a fresh clear, so there's no
		// reason to buffer more than one.
		VkImage GetDepthImage() const { return m_DepthImage; }
		VkImageView GetDepthImageView() const { return m_DepthImageView; }
		static VkFormat GetDepthFormat() { return kSharedDepthFormat; }
		// The depth's bindless slot (the deferred lighting pass samples it -
		// the framebuffer's depth-wrap precedent).
		uint32_t GetDepthBindlessIndex() const { return m_DepthBindlessIndex; }

		// The swapchain's fixed colour format (ChooseSurfaceFormat
		// prefers B8G8R8A8_UNORM - the same format the framebuffer spec
		// defaults to, which is what lets the renderers' pipelines stay
		// valid for both target kinds).
		static VkFormat GetColourFormat() { return VK_FORMAT_B8G8R8A8_UNORM; }

	private:
		void CreateSurface(GLFWwindow* window);
		void CreateSwapchain(uint32_t width, uint32_t height);
		void CreateImageViews();
		void CreateSyncObjects();
		void CreateDepthResources();
		void DestroySwapchainResources();
		// F1: the depth transition's own command pool - the device-wide
		// transient pool is game-thread-only (uploads + the asset batch),
		// while this runs on the SUBMIT thread during a resize; a
		// per-swapchain pool removes the cross-thread pool sharing
		// entirely.
		void CreateTransitionCommandPool();
		void DestroyTransitionCommandPool();

		VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
		VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
		VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height) const;

	private:
		VulkanDevice* m_Device = nullptr;
		GLFWwindow* m_Window = nullptr;

		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;

		VkFormat m_ImageFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D m_Extent{};
		VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
		uint32_t m_RequestedWidth = 0;
		uint32_t m_RequestedHeight = 0;

		VkImage m_DepthImage = VK_NULL_HANDLE;
		VmaAllocation m_DepthAllocation = VK_NULL_HANDLE;
		VkImageView m_DepthImageView = VK_NULL_HANDLE;
		// The depth's sample-read wrap (nearest sampler + bindless
		// slot) for the deferred lighting pass.
		VkSampler m_DepthSampler = VK_NULL_HANDLE;
		uint32_t m_DepthBindlessIndex = 0;
		VkCommandPool m_TransitionCommandPool = VK_NULL_HANDLE;

		// What ChoosePresentMode should prefer next time it runs - read by
		// CreateSwapchain, not just SetVsync, so the very first swapchain
		// (built from Init(), before anyone has called SetVsync) already
		// honours whatever GenericWindow::SetVSync(true) set as the default
		// in its own constructor.
		bool m_VsyncEnabled = true;

		std::vector<VkImage> m_Images;
		std::vector<VkImageView> m_ImageViews;
		std::vector<TinyEngine::rhi::TextureHandle> m_ImageHandles;

		// Render finished semaphores are indexed by swapchain image, not frame in
		// flight. Present operates on a specific image index, and acquiring images
		// doesn't strictly round robin against frame index once the swapchain has
		// more images than frames in flight, so tying this semaphore to the image
		// itself is what the Vulkan spec's own examples recommend to avoid a
		// semaphore being waited on twice before it was ever signalled again.
		std::vector<VkSemaphore> m_RenderFinishedSemaphores;

		// T-F-C: the bounded-acquire shutdown handshake + the game-thread
		// framebuffer-size publication (the submit thread never calls GLFW).
		std::atomic<bool> m_ShutdownRequested{ false };
		std::atomic<uint32_t> m_PublishedWidth{ 0 };
		std::atomic<uint32_t> m_PublishedHeight{ 0 };
		// 0 = vsync off, 1 = vsync on, 2 = consumed (no pending request).
		std::atomic<uint32_t> m_PendingVsync{ 2 };
		// 0 = none, 1 = out-of-date rebuild requested (consumed on the game
		// thread under the park).
		std::atomic<uint32_t> m_PendingRebuild{ 0 };

		// The image-available binary semaphore is per frame in flight (the
		// acquire/present handshake). The CPU-side reuse of a frame's command
		// pool/buffer is paced by the device's graphics-queue timeline: each
		// frame's last submit value (0 = never submitted) is waited on before
		// reusing the slot (P-A2 - the fences are gone).
		VkSemaphore m_ImageAvailableSemaphores[VulkanDevice::FramesInFlight] = {};
		uint64_t m_FrameLastValues[VulkanDevice::FramesInFlight] = {};

		// Tracks which frame's submit value last used a given swapchain image,
		// so an acquire that hands back a still-in-use image waits on that
		// value first instead of racing it (0 = never used).
		std::vector<uint64_t> m_ImagesInFlight;
	};

}
