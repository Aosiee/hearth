#pragma once

#include "hearth/Assert.h"
#include "hearth/rhi/GpuFrameDebugData.h"
#include "hearth/rhi/vulkan/VulkanWindowContext.h"

#include "hearth/rhi/vulkan/VulkanCommon.h"

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace hearth::platform { class Platform; class Window; }

namespace hearth::rhi {


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
		// The slot count is fixed (MaxWindowSlots windows at once).
		static constexpr uint32_t MaxWindowSlots = 4;
		static constexpr uint32_t TotalFrameSlots = FramesInFlight * MaxWindowSlots;

		// The per-(frame, thread) command-pool table's thread count.
		// Hardware concurrency, capped at 16, fixed at Init.
		static constexpr uint32_t MaxRecordThreads = 16;

		// Reserves the lowest free window slot and returns its base frame index
		// (slotIndex * FramesInFlight). Must be called between frames (a
		// window's creation), never while another window's
		// submission is in flight. Asserts when every slot is claimed.
		// The slot records its owning context until ReleaseWindowSlot (the
		// context's destructor), so the loop thread's parks can wait EVERY live
		// window, not only the main one.
		// A released slot is handed out again only after RecycleWindowSlot -
		// the old owner's destructor calls it once its last GPU work has
		// completed and its command buffers are freed - so the cap counts the
		// LIVE windows. The lowest free slot is taken.
		uint32_t AllocateWindowSlot(VulkanWindowContext* owner);
		void ReleaseWindowSlot(uint32_t slotBase);
		void RecycleWindowSlot(uint32_t slotBase);
		// The live window contexts, indexed by window slot (null = never
		// allocated or released). Loop-thread-only, like the allocation.
		const std::array<VulkanWindowContext*, MaxWindowSlots>& GetLiveWindowContexts() const { return m_WindowContexts; }

		// The main window's context, owned here, beside the window-slot
		// table. Set exactly once, by the Init of the context whose
		// IsMainWindow is true (the flag, whatever window slot it holds; a
		// secondary window never sets it); cleared by that
		// context's destructor. The clear is deliberately LATER than
		// ReleaseWindowSlot's table entry: the main context's own teardown
		// joins its submit thread, whose last record still reads the main
		// context. Written on the loop thread between frames; read on the
		// loop thread and inside record jobs.
		void SetMainContext(VulkanWindowContext* context);
		void ClearMainContext(VulkanWindowContext* context);
		VulkanWindowContext* TryGetMainContext() const { return m_MainContext; }
		// Asserts the main context exists (a null main context once the
		// main window is up is a bug, not a case).
		VulkanWindowContext& GetMainContext() const;

		// The loop thread (named the game thread here): the thread that
		// created the device (the first window's creation) and runs the
		// frame loop. The transient pool and the upload batch below are
		// loop-thread-only.
		bool IsGameThread() const { return std::this_thread::get_id() == m_GameThreadId; }

		// windowForExtensionQuery is only used to ask GLFW which instance extensions
		// this windowing system needs (X11/Wayland/Win32 surface support etc). No
		// surface is created here, that happens per window in VulkanSwapchain.
		// The names the Vulkan instance reports (drivers may key behaviour on
		// them). Set before Init.
		void SetApplicationInfo(std::string applicationName, std::string engineName)
		{
			m_ApplicationName = std::move(applicationName);
			m_EngineName = std::move(engineName);
		}
		void Init(hearth::platform::Window* windowForExtensionQuery);
		void Shutdown();

		VkInstance GetInstance() const { return m_Instance; }
		VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
		VkDevice GetDevice() const { return m_Device; }
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		uint32_t GetGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }

		// The queue is externally synchronized - with the frame's
		// submit/present on a dedicated submit thread, every
		// queue-touching call on any thread (the loop thread's
		// EndSingleTimeCommands uploads, the submit thread's
		// submit/present, the Recreate device-wait) holds this.
		std::mutex& GetQueueMutex() { return m_QueueMutex; }

		VmaAllocator GetAllocator() const { return m_Allocator; }

		// The command pools are keyed by (frame slot, record thread); no
		// pool belongs to a window slot, so adding recording threads moves
		// no pools.
		VkCommandPool GetCommandPool(uint32_t frameIndex, uint32_t threadIndex = 0) const
		{
			uint32_t poolIndex = frameIndex * m_RecordThreadCount + threadIndex;
			HEARTH_ASSERT(poolIndex < m_CommandPools.size() && m_CommandPools[poolIndex] != VK_NULL_HANDLE, "Command pool index out of range!");
			return m_CommandPools[poolIndex];
		}

		// ---- The graphics-queue timeline -----------------------------------
		// One timeline semaphore (no per-frame fences) does the pacing and
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

#if defined(HEARTH_GPU_DEBUG)
		// The GPU frame debugger's sink, the program's and optional (it must
		// outlive the device: the messenger and the submit thread write it).
		// Set before Init - the instance's chained messenger reports through it
		// (its pUserData). The record passes and the submit thread write
		// through GetGpuFrameDebugSink.
		void SetGpuFrameDebugSink(GpuFrameDebugSink* sink) { m_GpuFrameDebugSink = sink; }
		// The program's profiler (optional: no timing is reported without one).
		void SetGpuTimingSink(GpuTimingSink* sink) { m_GpuTimingSink = sink; }
		GpuFrameDebugSink& GetGpuFrameDebugSink() const
		{
			HEARTH_ASSERT(m_GpuFrameDebugSink, "VulkanDevice: the GPU frame debug sink was never set (set it before Init)");
			return *m_GpuFrameDebugSink;
		}
		// Null when the program set none.
		GpuFrameDebugSink* TryGetGpuFrameDebugSink() const { return m_GpuFrameDebugSink; }
#endif

		// Anisotropic filtering is an optional device feature, not something
		// every Vulkan implementation is required to support, unlike GL where
		// GL_TEXTURE_MAX_ANISOTROPY is just always there to query a limit from.
		// Checked once at device creation and requested if available; callers
		// (VulkanImage2D's sampler) need to check this rather than assume it's
		// on, requesting VK_TRUE without the feature enabled is a validation error.
		bool IsSamplerAnisotropySupported() const { return m_SamplerAnisotropySupported; }

		// Non-solid polygon modes (VK_POLYGON_MODE_LINE - a wireframe
		// pipeline) are an optional device feature too. Checked once
		// at device creation and requested if available; requesting LINE
		// without the feature enabled is VUID-...-polygonMode-01507.
		bool IsFillModeNonSolidSupported() const { return m_FillModeNonSolidSupported; }

		// Thin wrapper over vkSetDebugUtilsObjectNameEXT. Safe to call even when
		// validation/debug utils aren't active, it just becomes a no-op, so callers
		// don't need to guard every call site themselves. This is what makes a
		// RenderDoc/Nsight capture and a frame debugger show real names
		// instead of raw handles.
		void SetObjectName(VkObjectType type, uint64_t handle, const std::string& name) const;

		// Wraps a stretch of recorded commands in a named, coloured label. Shows up
		// as a group in RenderDoc/Nsight captures. Also where GPU timing for the
		// program's frame debugger comes from - every Begin/End call site gets
		// per-pass GPU timestamps.
		// timed = false records the label without a timestamp pair (the scope stack
		// carries the no-slot sentinel, as on an overflow).
		// `frameSlot` is the slot the buffer records for (its command list's
		// FrameSlot()): it picks the timestamp pool and the (slot, record thread)
		// scope stacks. Begin and End of one label pass the same slot.
		void CmdBeginDebugLabel(VkCommandBuffer commandBuffer, uint32_t frameSlot, const std::string& name, const float colour[4] = nullptr, bool timed = true);
		void CmdEndDebugLabel(VkCommandBuffer commandBuffer, uint32_t frameSlot);

		// Call once per application frame, right after that frame-in-flight
		// slot's timeline wait (VulkanSwapchain::AcquireNextImage) and before
		// any new recording for it begins. Reads back GPU timestamps from
		// the *previous* time this slot was used (guaranteed complete by
		// that wait) into GpuFrameDebugInfo, then clears this
		// slot's bookkeeping ready for new Begin/EndDebugLabel calls this frame.
		// isMainWindow gates the GpuFrameDebugInfo sink (the one the
		// program's frame debugger reads) - secondary windows still get
		// their own per-slot bookkeeping, they just don't publish into it.
		void BeginProfilingFrame(uint32_t frameIndex, bool isMainWindow = true);

		// Must be the first recorded command for this frame's command buffer,
		// after BeginProfilingFrame has already read back its previous
		// results — resetting a query pool that still holds unread results
		// would lose them.
		void CmdResetTimestampPool(VkCommandBuffer commandBuffer, uint32_t frameIndex);

		// The frame-indexed timestamp pool (the RHI's WriteTimestamp
		// routes through this).
		VkQueryPool GetTimestampPool(uint32_t frameIndex) const
		{
			HEARTH_ASSERT(frameIndex < m_TimestampPools.size() && m_TimestampPools[frameIndex] != VK_NULL_HANDLE, "Timestamp pool index out of range!");
			return m_TimestampPools[frameIndex];
		}

		// Used for one-off resource setup work that doesn't happen inside the
		// normal per-frame recording, buffer/texture uploads mainly. Loop
		// thread only (asserted - the transient pool and the upload batch
		// are not synchronized; the swapchain's depth transition uses the
		// swapchain's own pool). Allocates
		// from a dedicated transient pool (separate from the per-frame-in-flight
		// pools, which get reset wholesale every frame and would be the wrong
		// place for something that might outlive a single frame's turnaround),
		// records nothing itself, and blocks until the GPU has finished before
		// returning from EndSingleTimeCommands. That's an intentional
		// simplification: resource uploads happen rarely enough (texture/mesh
		// load, not every frame) that stalling for them is acceptable, an async
		// transfer queue can replace this later if upload volume ever justifies it.
		VkCommandBuffer BeginSingleTimeCommands();
		void EndSingleTimeCommands(VkCommandBuffer commandBuffer);

		// The upload batch. While a batch is active,
		// BeginSingleTimeCommands returns the batch's command buffer and
		// EndSingleTimeCommands is a no-op - every upload records into one
		// buffer.
		// EndUploadBatch ends + submits it once on the graphics queue
		// WITHOUT a device waitIdle (the queue's submission order + the
		// frame's acquire timeline wait order it before the frame that
		// samples the uploads). Loop thread only, like the single-time
		// commands (the batch is the m_UploadBatch member; every entry
		// point asserts the loop thread).
		// A batch with no recorded commands submits nothing.
		void BeginUploadBatch();
		void EndUploadBatch();

		// While a batch is active, upload helpers
		// hand their transient staging buffers here instead of destroying
		// them: the batch's recorded copies still reference them, and they
		// die only once the GPU has passed the batch's submission (the
		// timeline). Returns false when no batch is active - the caller
		// destroys the staging itself.
		bool BatchRetireStaging(VkBuffer buffer, VmaAllocation allocation);

	private:
		void CreateInstance(hearth::platform::Window* windowForExtensionQuery);
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
#if defined(HEARTH_GPU_DEBUG)
		GpuFrameDebugSink* m_GpuFrameDebugSink = nullptr; // the program's; outlives the device
		GpuTimingSink* m_GpuTimingSink = nullptr; // the program's; may be null
#endif
		std::string m_ApplicationName = "Hearth program";
		std::string m_EngineName = "Hearth";

		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;
		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		mutable std::mutex m_QueueMutex;
		uint32_t m_GraphicsQueueFamily = 0;

		VmaAllocator m_Allocator = VK_NULL_HANDLE;

		std::vector<VkCommandPool> m_CommandPools;
		uint32_t m_RecordThreadCount = 1;
		VkCommandPool m_TransientCommandPool = VK_NULL_HANDLE;

		// The graphics-queue timeline (see the accessors above).
		VkSemaphore m_TimelineSemaphore = VK_NULL_HANDLE;
		std::atomic<uint64_t> m_TimelineValue{ 0 };

		// Claimed from AllocateWindowSlot until RecycleWindowSlot.
		std::array<bool, MaxWindowSlots> m_WindowSlotClaimed{};
		std::array<VulkanWindowContext*, MaxWindowSlots> m_WindowContexts{};
		VulkanWindowContext* m_MainContext = nullptr;

		std::thread::id m_GameThreadId;
		void AssertGameThread(const char* entryPoint) const;

		// The open upload batch. While Buffer is held, every
		// Begin/EndSingleTimeCommands pair records into it instead of
		// allocating its own. Submitted buffers + their retired staging
		// are reclaimed once the timeline passes the batch's signal.
		struct UploadBatch
		{
			VkCommandBuffer Buffer = VK_NULL_HANDLE;
			bool Dirty = false;
			std::vector<VkCommandBuffer> SubmittedBuffers;
			uint64_t SubmittedValue = 0;
			std::vector<std::pair<VkBuffer, VmaAllocation>> PendingStaging;
		};
		UploadBatch m_UploadBatch;

		bool m_ValidationEnabled = false;
		bool m_SamplerAnisotropySupported = false;
		bool m_FillModeNonSolidSupported = false;
		// The Offscreen mode (the null window) creates the device without
		// the surface/swapchain extensions and without the
		// present-capability requirement.
		bool m_SurfaceEnabled = true;
		// The window system the surfaces come from (null in the Offscreen
		// mode, which has none).
		const hearth::platform::Platform* m_Platform = nullptr;
		// GPU frame debugger timing.
		// One query pool per frame slot (window slot * FramesInFlight), same
		// reasoning as the command pools: reusing slot N is only safe once
		// frame N's timeline value has passed, which BeginProfilingFrame's
		// caller already guarantees.
		// One TOP+BOTTOM timestamp pair per GPU scope; a UI renderer that
		// labels every batch (every clip rect + pipeline change its own
		// scope) can split into dozens alone, so this is generous rather
		// than minimal. The per-frame query pools are sized
		// from it and are cheap.
		static constexpr uint32_t kMaxTimestampsPerFrame = 1024;
		static constexpr uint32_t kNoTimestampSlot = ~0u; // a label recorded without timing
		std::atomic<bool> m_TimestampOverflowWarned{ false };
		void NoteTimestampOverflow();

		struct GpuScopeRecord
		{
			std::string Name;
			uint32_t BeginQuery;
			uint32_t EndQuery;
			uint32_t ProfilerFrameIndex = 0; // the submit-time frame
		};

		bool m_TimestampsSupported = false;
		float m_TimestampPeriodNs = 1.0f;
		uint64_t m_TimestampMask = ~0ull; // timestampValidBits

		// The device clock calibrated against the profiler's steady clock
		// (VK_KHR/EXT_calibrated_ timestamps), resampled every
		// kCalibrationInterval readbacks. Without the extension (or a
		// matching host domain) the passes go out on the device clock and
		// the timeline anchors them.
		static constexpr uint32_t kCalibrationInterval = 64;
		const char* m_CalibrationExtension = nullptr; // enabled at device creation
		VkTimeDomainKHR m_HostTimeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
		PFN_vkGetCalibratedTimestampsKHR m_GetCalibratedTimestamps = nullptr;
		bool m_GpuClockValid = false;
		GpuClockMapping m_GpuClock;
		uint32_t m_CalibrationCountdown = 0;
		double m_GpuPeriodEstimate = 0.0; // the measured ns per tick (0 = not yet)
		void DetectCalibratedTimestamps();
		void CalibrateGpuClock();
		uint32_t m_MaxPushConstantsSize = 128;
		// The device's identity (for a program's capture metadata).
		std::string m_DeviceName;
		uint32_t m_DriverVersion = 0;
		uint32_t m_ApiVersion = 0;
		uint32_t m_VendorId = 0;
	public:
		float GetTimestampPeriodNs() const { return m_TimestampPeriodNs; }
		uint32_t GetMaxPushConstantsSize() const { return m_MaxPushConstantsSize; }
		// The device's identity (for a program's capture metadata).
		const std::string& GetDeviceName() const { return m_DeviceName; }
		uint32_t GetDriverVersion() const { return m_DriverVersion; }
		uint32_t GetApiVersion() const { return m_ApiVersion; }
		uint32_t GetVendorId() const { return m_VendorId; }
		uint32_t GetRecordThreadCount() const { return m_RecordThreadCount; }
	private:

		// The calling thread's record index (its loom thread index),
		// clamped into [0, m_RecordThreadCount). The program configures its
		// loom pool's workers 1:1 with the record pools, so this maps
		// uniquely; a thread outside the range shares the clamp
		// defensively (and asserts in dev).
		uint32_t RecordThreadIndex() const;

		std::vector<VkQueryPool> m_TimestampPools;
		// Per-frame slot counters (atomic: the parallel record workers
		// allocate timestamp slots concurrently; the slots stay contiguous
		// so the readback reads one prefix).
		std::unique_ptr<std::atomic<uint32_t>[]> m_NextQuerySlot;
		// Per-(frame slot, record thread): the scope begin/end pairing is
		// always recorded by one worker, so the stacks are per-thread.
		std::vector<std::vector<uint32_t>> m_ScopeBeginStack;
		std::vector<std::vector<std::string>> m_ScopeNameStack;
		std::vector<std::vector<GpuScopeRecord>> m_RecordedScopes;
	};

}
