#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hearth::rhi {

	// The device-tick -> CPU-ns mapping from one calibration sample (a
	// device tick and the CPU ns read at the same instant). TickMask is the
	// timestampValidBits mask: the delta wraps within it, and a tick before
	// the sample maps to an earlier time.
	struct GpuClockMapping
	{
		uint64_t DeviceTick = 0;
		uint64_t CpuNs = 0;
		double PeriodNs = 1.0;
		uint64_t TickMask = ~0ull;

		uint64_t ToCpuNs(uint64_t tick) const
		{
			uint64_t delta = (tick - DeviceTick) & TickMask;
			int64_t signedTicks = (int64_t)delta;
			if (TickMask != ~0ull && delta > (TickMask >> 1))
				signedTicks = (int64_t)delta - (int64_t)(TickMask + 1);
			const double ns = (double)CpuNs + (double)signedTicks * PeriodNs;
			return ns <= 0.0 ? 0ull : (uint64_t)(ns + 0.5);
		}
	};


	// Everything a frame debugger shows. Plain data, no Vulkan types, so
	// the program can read and render it without a single Vulkan include
	// (a cross-backend query, populated by the backend). Only ever
	// populated once the renderer has actually completed a frame; a
	// snapshot taken before then comes back with Available = false.

	struct GpuPassTiming
	{
		std::string Name;
		float GpuTimeMs = 0.0f;
	};

	// One VMA heap's live budget (the per-heap usage/budget
	// reported by the allocator at frame end).
	struct GpuHeapBudget
	{
		uint64_t BytesUsed = 0;
		uint64_t BytesBudget = 0;
	};

	struct GpuBarrierRecord
	{
		std::string Name;       // the pass the transition is attributed to
		std::string OldLayout;  // the graph-state vocabulary
		std::string NewLayout;
	};

	struct GpuFrameDebugInfo
	{
		bool Available = false;

		// Timings are for the most recently *completed* frame, not the one
		// currently being recorded — GPU timestamps aren't readable until the
		// frame that wrote them has actually finished executing, see
		// VulkanDevice::BeginProfilingFrame.
		std::vector<GpuPassTiming> PassTimings;

		// The derived barriers of the frame's compiled RenderGraph(s) - one
		// record per state transition, attributed to the pass that needs it.
		std::vector<GpuBarrierRecord> Barriers;

		uint32_t FrameInFlightIndex = 0;
		uint32_t SwapchainImageIndex = 0;
		std::string PresentMode;

		// VMA heap budgets for the most recently completed frame.
		std::array<GpuHeapBudget, 16> HeapBudgets;
		uint32_t HeapBudgetCount = 0;

		// Most recent messages first. Capped, see
		// VulkanDevice's DebugMessengerCallback for the limit.
		std::vector<std::string> ValidationMessages;
	};

	// The frame debug info is written by the submit thread (and the
	// validation-messenger callback on whichever thread triggered a call)
	// and read by the program's frame debugger on the loop thread -
	// writers hold the sink's lock, the reader takes a snapshot.
	//
	// The program owns the sink (VulkanDevice::SetGpuFrameDebugSink) and
	// must keep it alive longer than the device. The messenger reaches it
	// through its pUserData, the submit thread and the record passes
	// through their VulkanDevice, the program through Snapshot.
	//
	// The type and every population site are compiled out without
	// HEARTH_GPU_DEBUG (a release program has no frame debugger).
#if defined(HEARTH_GPU_DEBUG)
	class GpuFrameDebugSink
	{
	public:
		// The reader's copy, taken under the lock.
		GpuFrameDebugInfo Snapshot() const
		{
			std::lock_guard lock(m_Mutex);
			return m_Info;
		}

		// A writer's scope: the lock is held for the Writer's lifetime.
		struct Writer
		{
			std::unique_lock<std::mutex> Lock;
			GpuFrameDebugInfo& Info;
		};
		Writer Write() { return Writer{ std::unique_lock<std::mutex>(m_Mutex), m_Info }; }

	private:
		mutable std::mutex m_Mutex;
		GpuFrameDebugInfo m_Info;
	};

	// Where the device reports GPU timing to the program's profiler: each
	// timed scope's begin and end (in the profiler's clock when the GPU
	// clock is calibrated, else in device-clock nanoseconds), the frame it
	// was submitted in, and the calibration's quality as plotted values.
	// Set by the program; without one nothing is reported.
	class GpuTimingSink
	{
	public:
		virtual ~GpuTimingSink() = default;
		// The program's frame index, stamped on each scope at record time.
		virtual uint32_t CurrentFrameIndex() const = 0;
		virtual void PlotValue(const char* name, double value) = 0;
		virtual void GpuPassPair(uint64_t beginNs, uint64_t endNs, uint32_t frameIndex, const char* name, bool calibrated) = 0;
	};
#endif // HEARTH_GPU_DEBUG


}
