#pragma once

#include "Platform/Vulkan/VulkanCommon.h"

#include <atomic>

struct GLFWwindow;

namespace TinyEngine {

	// Owns everything about the Vulkan backend that exists once for the whole
	// application: the instance, the physical/logical device pair, the
	// graphics queue, the VMA allocator, and one command pool per frame in flight.
	//
	// Nothing in here is tied to a particular window or swapchain, that lives in
	// VulkanSwapchain. A window just borrows this device to build its swapchain
	// against.
	class VulkanDevice
	{
	public:
		// How many frames worth of command pools/buffers are kept alive at once,
		// so recording frame N+1 never has to wait on frame N's GPU work to
		// actually finish first. Two is a plain double buffer; going higher
		// trades a little latency for smoother pacing when frame times are
		// uneven.
		static constexpr uint32_t FramesInFlight = 2;

		// Multi-window slotting: the shared device hands each window a *slot*
		// of FramesInFlight per-frame resources (command pools, timestamp
		// pools, profiling bookkeeping). The frame-index space every
		// per-frame-in-flight array in the backend is sized against is
		// TotalFrameSlots; a window's frame N lives at slot*FramesInFlight+N.
		// The fixed slot count is a v1 simplification - per-window ownership
		// replaces the slotting wholesale rather than this becoming dynamic.
		static constexpr uint32_t MaxWindowSlots = 4;
		static constexpr uint32_t TotalFrameSlots = FramesInFlight * MaxWindowSlots;

		// The per-(frame, thread) command-pool table's thread count.
		// Hardware concurrency, capped at 16, fixed at Init.
		static constexpr uint32_t MaxRecordThreads = 16;

		// Reserves the next window slot and returns its base frame index
		// (slotIndex * FramesInFlight). Must be called between frames (a
		// window's VulkanContext::Init), never while another window's
		// submission is in flight. Asserts when the cap is exceeded.
		uint32_t AllocateWindowSlot();

		// windowForExtensionQuery is only used to ask GLFW which instance extensions
		// this windowing system needs (X11/Wayland/Win32 surface support etc). No
		// surface is created here, that happens per window in VulkanSwapchain.
		void Init(GLFWwindow* windowForExtensionQuery);
		void Shutdown();

		VkInstance GetInstance() const { return m_Instance; }
		VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
		VkDevice GetDevice() const { return m_Device; }
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		uint32_t GetGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }
		VmaAllocator GetAllocator() const { return m_Allocator; }

		// The command pools are keyed by (frame slot, record thread); no
		// pool belongs to a window slot, so adding recording threads moves
		// no pools.
		VkCommandPool GetCommandPool(uint32_t frameIndex, uint32_t threadIndex = 0) const
		{
			uint32_t poolIndex = frameIndex * m_RecordThreadCount + threadIndex;
			TE_CORE_ASSERT(poolIndex < m_CommandPools.size() && m_CommandPools[poolIndex] != VK_NULL_HANDLE, "Command pool index out of range!");
			return m_CommandPools[poolIndex];
		}

		// ---- P-A2: the graphics-queue timeline -----------------------------
		// One timeline semaphore replaces the per-frame fences for pacing and
		// in-flight tracking; binary semaphores remain ONLY for swapchain
		// acquire/present. Every submission signals the timeline at a fresh
		// value; waiters block until the GPU passes it.
		uint64_t AllocateTimelineValue();          // the next value a submission will signal
		void AdvanceTimelineValue(uint64_t value); // callers signalling an explicit value keep the allocator monotonic
		uint64_t GetLastSubmittedValue() const { return m_TimelineValue.load(); }
		VkSemaphore GetTimelineSemaphore() const { return m_TimelineSemaphore; }
		void WaitForTimelineValue(uint64_t value); // blocks the CALLING thread
		uint64_t GetCompletedTimelineValue();      // the value the GPU has actually reached

		bool IsValidationEnabled() const { return m_ValidationEnabled; }

		// Anisotropic filtering is an optional device feature, not something
		// every Vulkan implementation is required to support, unlike GL where
		// GL_TEXTURE_MAX_ANISOTROPY is just always there to query a limit from.
		// Checked once at device creation and requested if available; callers
		// (VulkanTexture2D's sampler) need to check this rather than assume it's
		// on, requesting VK_TRUE without the feature enabled is a validation error.
		bool IsSamplerAnisotropySupported() const { return m_SamplerAnisotropySupported; }

		// Thin wrapper over vkSetDebugUtilsObjectNameEXT. Safe to call even when
		// validation/debug utils aren't active, it just becomes a no-op, so callers
		// don't need to guard every call site themselves. This is what makes a
		// RenderDoc/Nsight capture and the in-editor GPU frame debugger show real
		// names instead of raw handles.
		void SetObjectName(VkObjectType type, uint64_t handle, const std::string& name) const;

		// Wraps a stretch of recorded commands in a named, coloured label. Shows up
		// as a group in RenderDoc/Nsight captures. Also where GPU timing for the
		// in-editor GPU frame debugger comes from - every Begin/End call site
		// gets per-pass GPU timestamps.
		void CmdBeginDebugLabel(VkCommandBuffer commandBuffer, const std::string& name, const float colour[4] = nullptr);
		void CmdEndDebugLabel(VkCommandBuffer commandBuffer);

		// Call once per application frame, right after that frame-in-flight
		// slot's fence has been waited on (VulkanSwapchain::AcquireNextImage)
		// and before any new recording for it begins. Reads back GPU
		// timestamps from the *previous* time this slot was used (guaranteed
		// complete by the fence wait) into GpuFrameDebugInfo, then clears this
		// slot's bookkeeping ready for new Begin/EndDebugLabel calls this frame.
		// isMainWindow gates the GpuFrameDebugInfo sink (a single global the
		// editor's GPU frame debugger reads) - secondary windows still get
		// their own per-slot bookkeeping, they just don't publish into it.
		void BeginProfilingFrame(uint32_t frameIndex, bool isMainWindow = true);

		// Must be the first recorded command for this frame's command buffer,
		// after BeginProfilingFrame has already read back its previous
		// results — resetting a query pool that still holds unread results
		// would lose them.
		void CmdResetTimestampPool(VkCommandBuffer commandBuffer, uint32_t frameIndex);

		// The frame-indexed timestamp pool (the RHI's WriteTimestamp
		// routes through this; the P-A2 frame-sync rework re-keys it).
		VkQueryPool GetTimestampPool(uint32_t frameIndex) const
		{
			TE_CORE_ASSERT(frameIndex < m_TimestampPools.size() && m_TimestampPools[frameIndex] != VK_NULL_HANDLE, "Timestamp pool index out of range!");
			return m_TimestampPools[frameIndex];
		}

		// Not tied to the command buffer at all (this is host-side
		// bookkeeping only), used to populate the GPU frame debugger's
		// barrier list. Only called at the handful of always-present,
		// per-frame barrier sites (swapchain clear, editor viewport target),
		// not one-off resource upload barriers — see GpuFrameDebugData.h.
		void RecordBarrierForDebug(const std::string& resourceName, VkImageLayout oldLayout, VkImageLayout newLayout) const;

		// Used for one-off resource setup work that doesn't happen inside the
		// normal per-frame recording, buffer/texture uploads mainly. Allocates
		// from a dedicated transient pool (separate from the per-frame-in-flight
		// pools, which get reset wholesale every frame and would be the wrong
		// place for something that might outlive a single frame's turnaround),
		// records nothing itself, and blocks until the GPU has finished before
		// returning from EndSingleTimeCommands. That's an intentional
		// simplification: resource uploads happen rarely enough (texture/mesh
		// load, not every frame) that stalling for them is fine for now, an async
		// transfer queue can replace this later if upload volume ever justifies it.
		VkCommandBuffer BeginSingleTimeCommands() const;
		void EndSingleTimeCommands(VkCommandBuffer commandBuffer) const;

	private:
		void CreateInstance(GLFWwindow* windowForExtensionQuery);
		void CreateDebugMessenger();
		void PickPhysicalDevice();
		void CreateLogicalDevice();
		void CreateAllocator();
		void CreateCommandPools();
		void CreateTimestampQueryPools();
		void CreateTimelineSemaphore();

		bool CheckValidationLayerSupport() const;

	private:
		VkInstance m_Instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;

		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		uint32_t m_GraphicsQueueFamily = 0;

		VmaAllocator m_Allocator = VK_NULL_HANDLE;

		std::vector<VkCommandPool> m_CommandPools;
		uint32_t m_RecordThreadCount = 1;
		VkCommandPool m_TransientCommandPool = VK_NULL_HANDLE;

		// P-A2: the graphics-queue timeline (see the accessors above).
		VkSemaphore m_TimelineSemaphore = VK_NULL_HANDLE;
		std::atomic<uint64_t> m_TimelineValue{ 0 };

		uint32_t m_NextWindowSlot = 0;

		bool m_ValidationEnabled = false;
		bool m_SamplerAnisotropySupported = false;

		// GPU frame debugger timing.
		// One query pool per frame slot (window slot * FramesInFlight), same
		// reasoning as the command pools: reusing slot N is only safe once
		// frame N's fence has signalled, which BeginProfilingFrame's caller
		// already guarantees.
		static constexpr uint32_t kMaxTimestampsPerFrame = 64;

		struct GpuScopeRecord
		{
			std::string Name;
			uint32_t BeginQuery;
			uint32_t EndQuery;
			uint32_t ProfilerFrameIndex = 0; // the submit-time frame
		};

		bool m_TimestampsSupported = false;
		float m_TimestampPeriodNs = 1.0f;
		uint32_t m_MaxPushConstantsSize = 128;
	public:
		float GetTimestampPeriodNs() const { return m_TimestampPeriodNs; }
		uint32_t GetMaxPushConstantsSize() const { return m_MaxPushConstantsSize; }
	private:

		std::vector<VkQueryPool> m_TimestampPools;
		std::vector<uint32_t> m_NextQuerySlot;
		std::vector<std::vector<uint32_t>> m_ScopeBeginStack;
		std::vector<std::vector<std::string>> m_ScopeNameStack;
		std::vector<std::vector<GpuScopeRecord>> m_RecordedScopes;
	};

}
