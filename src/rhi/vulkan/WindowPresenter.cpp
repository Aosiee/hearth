#include "hearth/rhi/vulkan/WindowPresenter.h"

#include "hearth/Assert.h"
#include "hearth/Log.h"
#include "hearth/Zone.h"
#include "hearth/platform/Window.h"
#include "hearth/rhi/GpuFrameDebugData.h"
#include "hearth/rhi/vulkan/VulkanCommon.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"

#include <algorithm>
#include <string>

namespace hearth::rhi {

	namespace {

		TextureState DepthLayoutToState(VkImageLayout layout)
		{
			switch (layout)
			{
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return TextureState::ShaderRead;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return TextureState::RenderTarget;
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return TextureState::DepthWrite;
			case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: return TextureState::DepthRead;
			default: return TextureState::Undefined;
			}
		}

		VkImageLayout StateToLayout(TextureState state)
		{
			switch (state)
			{
			case TextureState::ShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case TextureState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case TextureState::DepthWrite: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			case TextureState::DepthRead: return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
			default: return VK_IMAGE_LAYOUT_UNDEFINED;
			}
		}

	} // namespace

	rg::RGTexture SwapchainGraph::Depth()
	{
		if (!m_DepthImported)
		{
			m_Depth = m_Presenter.ImportDepth(m_Graph);
			m_DepthImported = true;
		}
		return m_Depth;
	}

	hearth::platform::Window* SwapchainGraph::Window() const
	{
		return m_Presenter.GetWindow();
	}

	VkExtent2D SwapchainGraph::Extent() const
	{
		return m_Presenter.m_Swapchain.GetExtent();
	}

	void SwapchainGraph::BeginRendering(VulkanCommandList& list, bool clear) const
	{
		m_Presenter.BeginSwapchainRendering(list, m_ImageIndex, clear);
	}

	WindowPresenter::WindowPresenter(VulkanRhiDevice& device, const WindowPresenterDesc& desc)
		: m_RhiDevice(device)
		, m_Device(device.GetVulkan())
		, m_Window(desc.Window)
		, m_Owner(desc.Owner)
		, m_InternName(desc.InternName)
		, m_IsMainWindow(desc.IsMainWindow)
		, m_Offscreen(desc.Window == nullptr)
		, m_OffscreenWidth(desc.OffscreenWidth)
		, m_OffscreenHeight(desc.OffscreenHeight)
	{
		HEARTH_ZONE_FUNCTION();
		HEARTH_ASSERT(m_Device->IsGameThread(), "a WindowPresenter is made off the loop thread (the thread that created the device)");

		if (!m_Offscreen)
			m_Swapchain.Init(*m_Device, m_RhiDevice, m_Window);

		// The window's frame slots on the shared device: command pools and
		// every per-frame-in-flight resource it touches live in
		// [base, base + FramesInFlight).
		m_WindowSlotBase = m_Device->AllocateWindowSlot(this);
		// The slots join the descriptor log's expected set (their first
		// apply re-seeds the whole set).
		m_RhiDevice.NoteSlotExpected(m_WindowSlotBase);

		// Main-ness is the window's declared flag, never a slot or an order.
		if (m_IsMainWindow)
			m_Device->SetMainContext(this);

		// The submit thread acquires, records, submits and presents, one
		// frame at a time and in order; it never steals other work.
		m_SubmitThread = loom::RegisterDedicatedThread({ .name = "Frame Submit", .idle = loom::DedicatedConfig::Idle::Sleep });
		for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
		{
			m_FrameSubmissions[i].Presenter = this;
			m_FrameSubmissions[i].Complete = loom::CreateCounter(0);
		}
	}

	void WindowPresenter::Stop()
	{
		if (m_Stopped)
			return;
		m_Stopped = true;

		// Leave the live-window table first: nothing may park on a window
		// that is shutting down.
		m_Device->ReleaseWindowSlot(m_WindowSlotBase);

		// The last graphics timeline value this window submitted (final once
		// the submit thread is joined).
		uint64_t lastSubmittedValue = 0;
		if (!m_Offscreen)
		{
			// The bounded acquire and the shutdown flag let the join interrupt
			// an in-flight frame.
			m_Swapchain.RequestShutdown();
			if (m_SubmitThread != loom::kInvalidDedicated)
				loom::UnregisterDedicatedThread(m_SubmitThread);
			for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
				lastSubmittedValue = std::max(lastSubmittedValue, m_Swapchain.GetFrameSubmitValue(i));
			m_Swapchain.Shutdown();
		}
		else
		{
			if (m_SubmitThread != loom::kInvalidDedicated)
				loom::UnregisterDedicatedThread(m_SubmitThread);
			for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
				lastSubmittedValue = std::max(lastSubmittedValue, m_FrameLastValues[i].load());
		}
		m_SubmitThread = loom::kInvalidDedicated;

		// The window's per-slot GPU state retires before the slot can go to a
		// new window: the new owner's first pool reset waits only its own
		// timeline values, so the last submission is waited here, the
		// per-pass buffers are freed, and the retire entries tagged to these
		// slots become freeable (their tag meant "this slot's earlier
		// submissions are done", which the wait guarantees).
		if (lastSubmittedValue != 0)
			m_Device->WaitForTimelineValue(lastSubmittedValue);
		for (FrameSubmission& submission : m_FrameSubmissions)
		{
			for (const auto& [pool, buffer] : submission.RetireCommandBuffers)
				vkFreeCommandBuffers(m_Device->GetDevice(), pool, 1, &buffer);
			submission.RetireCommandBuffers.clear();
		}
		for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
		{
			for (uint64_t r = 0; r < VulkanRhiDevice::kRetireResetDistance; r++)
				m_RhiDevice.NoteSlotReset(m_WindowSlotBase + i);
		}
	}

	WindowPresenter::~WindowPresenter()
	{
		Stop();

		if (IsValid(m_DepthHandle))
			m_RhiDevice.DestroyTexture(m_DepthHandle);
		m_DepthHandle = {};

		// The main window clears the device's main context on the way out
		// (after its submit thread's join: the last record still reads it).
		if (m_IsMainWindow)
			m_Device->ClearMainContext(this);

		// The slots leave the descriptor log's active set (their stale seqs
		// must not hold its floor back), then the slot is free for the next
		// window (its sets re-seed at the new owner's first apply).
		m_RhiDevice.DeactivateWindowSlots(m_WindowSlotBase);
		m_Device->RecycleWindowSlot(m_WindowSlotBase);
	}

	void WindowPresenter::SetClients(std::span<FrameClient* const> clients)
	{
		m_Clients.assign(clients.begin(), clients.end());
	}

	uint32_t WindowPresenter::GetCurrentFrameSlot() const
	{
		HEARTH_ASSERT(IsLoopThread(), "GetCurrentFrameSlot() off the loop thread - the frame index is the loop thread's");
		return m_WindowSlotBase + m_CurrentFrame;
	}

	uint32_t WindowPresenter::GetPreviousFrameSlot() const
	{
		HEARTH_ASSERT(IsLoopThread(), "GetPreviousFrameSlot() off the loop thread - the frame index is the loop thread's");
		return m_WindowSlotBase + (m_CurrentFrame + 1) % VulkanDevice::FramesInFlight;
	}

	uint64_t WindowPresenter::GetFrameSubmitValueForSlot(uint32_t slot) const
	{
		HEARTH_ASSERT(IsLoopThread(), "GetFrameSubmitValueForSlot() off the loop thread");
		HEARTH_ASSERT(slot >= m_WindowSlotBase && slot < m_WindowSlotBase + VulkanDevice::FramesInFlight,
			"GetFrameSubmitValueForSlot() with a slot outside this window's range");
		const uint32_t frameIndex = slot - m_WindowSlotBase;
		if (m_Offscreen)
			return m_FrameLastValues[frameIndex];
		return m_Swapchain.GetFrameSubmitValue(frameIndex);
	}

	WindowFrame WindowPresenter::MakeWindowFrame(uint32_t slot) const
	{
		WindowFrame frame;
		frame.WindowIndex = GetWindowIndex();
		frame.Slot = slot;
		frame.SlotBase = m_WindowSlotBase;
		frame.IsMain = m_IsMainWindow;
		return frame;
	}

	void WindowPresenter::ParkForClients()
	{
		// A reload swaps pipelines or code under every window's records, so
		// the park covers every live window (the first window to reach it may
		// be a secondary one, whose own idle left the main window recording).
		std::vector<FrameClient*> wanting;
		for (FrameClient* client : m_Clients)
		{
			if (client && client->WantsPark())
				wanting.push_back(client);
		}
		if (wanting.empty())
			return;
		WaitForAllRecordIdle(*m_Device);
		WaitForAllSubmitIdle(*m_Device);
		for (FrameClient* client : wanting)
			client->Parked();
	}

	void WindowPresenter::WaitForSlot()
	{
		// The slot-free condition: the next frame's update writes slot
		// (m_CurrentFrame + 1) % FramesInFlight, whose previous reader was
		// two frames back; its record must be done. BlockOnly: the record
		// belongs to the loom workers, so the loop thread does not help.
		FrameSubmission& previous = m_FrameSubmissions[(m_CurrentFrame + 1) % VulkanDevice::FramesInFlight];
		if (previous.RecordDone != loom::kInvalidCounter)
		{
			loom::WaitFor(previous.RecordDone, loom::Help::BlockOnly);
			loom::DestroyCounter(previous.RecordDone);
			previous.RecordDone = loom::kInvalidCounter;
		}
	}

	void WindowPresenter::Enqueue()
	{
		FrameSubmission& frame = m_FrameSubmissions[m_CurrentFrame];
		frame.Slot = m_WindowSlotBase + m_CurrentFrame;
		frame.FrameIndex = m_CurrentFrame;
		frame.RecordDone = loom::CreateCounter(1);
		// The frame's data is final: it goes into the slot before the submit
		// thread records it.
		const WindowFrame windowFrame = MakeWindowFrame(frame.Slot);
		for (FrameClient* client : m_Clients)
		{
			if (client)
				client->Publish(windowFrame);
		}
		loom::Signal(frame.Complete, -1); // one more frame in flight (0 -> 1)
#if defined(HEARTH_GPU_DEBUG)
		loom::Submit(loom::TaskDesc{ .fn = &WindowPresenter::FrameSubmitTask,
			.userData = &frame,
			.affinity = loom::Affinity::Dedicated,
			.thread = m_SubmitThread,
			.nameId = m_InternName ? m_InternName("Frame Submit") : 0,
			.window = (uint8_t)m_WindowSlotBase });
#else
		loom::Submit(loom::TaskDesc{ .fn = &WindowPresenter::FrameSubmitTask,
			.userData = &frame,
			.affinity = loom::Affinity::Dedicated,
			.thread = m_SubmitThread });
#endif

		// The retire queue is the loop thread's: the submit of the frame two
		// back happened before its RecordDone, which WaitForSlot waited.
		m_RhiDevice.DrainRetireQueue();
		m_CurrentFrame = (m_CurrentFrame + 1) % VulkanDevice::FramesInFlight;
	}

	void WindowPresenter::Present()
	{
		HEARTH_ZONE_FUNCTION();

		if (m_Offscreen)
		{
			// No surface, no present: the frame keeps the park, the update,
			// the slot-free wait, the enqueue and the retire drain.
			ParkForClients();
			const FrameUpdate update{ *this, nullptr, m_OffscreenWidth, m_OffscreenHeight, m_WindowSlotBase + m_CurrentFrame, GetWindowIndex(), true };
			for (FrameClient* client : m_Clients)
			{
				if (client)
					client->Update(update);
			}
			WaitForSlot();
			Enqueue();
			return;
		}

		const auto [width, height] = m_Window->FramebufferSize();

		// A minimised window has nothing to draw or present to.
		if (width == 0 || height == 0)
			return;

		// The loop thread publishes the framebuffer size and performs every
		// swapchain rebuild, under the record and submit park, so the
		// rebuild's handle-table work never races the loop thread's own
		// texture work or the submit thread's acquire and present.
		m_Swapchain.PublishFramebufferSize((uint32_t)width, (uint32_t)height);

		ParkForClients();

		{
			const VkExtent2D extent = m_Swapchain.GetExtent();
			const bool sizeDrift = (extent.width != (uint32_t)width || extent.height != (uint32_t)height);
			const bool rebuildRequested = m_Swapchain.ConsumeRebuildRequest();
			const bool vsyncPending = m_Swapchain.HasPendingVsync();

			if (sizeDrift || rebuildRequested || vsyncPending)
			{
				// The rebuild destroys the image views a record may still be
				// using: park the records first, then the submits.
				WaitForRecordIdle();
				WaitForSubmitIdle();
			}

			// A present-mode change can rebuild too; it runs under the same
			// park, before the size check.
			m_Swapchain.ApplyPendingVsync();

			if (sizeDrift || rebuildRequested)
			{
				// An out-of-date request re-queries the surface; a size drift
				// rebuilds to the published size.
				if (rebuildRequested && !sizeDrift)
					m_Swapchain.RecreateFromSurfaceCapabilities();
				else
					m_Swapchain.Recreate((uint32_t)width, (uint32_t)height);
			}

			EnsureDepthHandle();
		}

		const VkExtent2D extent = m_Swapchain.GetExtent();
		const FrameUpdate update{ *this, m_Window, extent.width, extent.height, m_WindowSlotBase + m_CurrentFrame, GetWindowIndex(), false };
		for (FrameClient* client : m_Clients)
		{
			if (client)
				client->Update(update);
		}

		WaitForSlot();
		Enqueue();
	}

	void WindowPresenter::EnsureDepthHandle()
	{
		// The depth's barrier-only graph handle. The swapchain recreates the
		// image underneath; the handle re-adopts when the depth's generation
		// moves (which also covers a recreate at the same extent - a stale
		// handle's barrier on a destroyed image crashed).
		if (IsValid(m_DepthHandle) && m_AdoptedDepthGeneration == m_Swapchain.GetDepthGeneration())
			return;
		if (IsValid(m_DepthHandle))
		{
			// Replacing a live handle parks this window first (a no-op when
			// the rebuild already parked).
			WaitForRecordIdle();
			WaitForSubmitIdle();
			m_RhiDevice.DestroyTexture(m_DepthHandle);
		}
		m_DepthHandle = m_RhiDevice.AdoptExternalImage((void*)m_Swapchain.GetDepthImage());
		m_DepthLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; // the creation-time transition
		m_AdoptedDepthGeneration = m_Swapchain.GetDepthGeneration();
	}

	rg::RGTexture WindowPresenter::ImportDepth(rg::RenderGraph& graph)
	{
		HEARTH_ASSERT(IsValid(m_DepthHandle), "the swapchain depth handle is missing at record (Present adopts it)");
		return graph.Import(m_DepthHandle, DepthLayoutToState(m_DepthLayout));
	}

	void WindowPresenter::FrameSubmitTask(void* userData)
	{
		FrameSubmission* frame = static_cast<FrameSubmission*>(userData);
		frame->Presenter->ProcessSubmittedFrame(*frame);
	}

	// One pass's record (a loom worker task): the worker records into its own
	// (frame slot, thread) command pool, so no two workers share one.
	void WindowPresenter::RecordPassTask(void* userData)
	{
		RecordJob& job = *static_cast<RecordJob*>(userData);

		// The pools are 1:1 with loom's threads; the assert catches a
		// mismatch the release-only clamp would hide.
		const int rawThreadIndex = loom::CurrentThreadIndex();
		HEARTH_ASSERT(rawThreadIndex < (int)job.Device->GetRecordThreadCount(),
			"the loom thread index exceeds the record command-pool count");
		const uint32_t threadIndex = (uint32_t)std::min<int>(rawThreadIndex, (int)job.Device->GetRecordThreadCount() - 1);

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = job.Device->GetCommandPool(job.Slot, threadIndex);
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		VkCommandBuffer passBuffer = VK_NULL_HANDLE;
		VK_CHECK(vkAllocateCommandBuffers(job.Device->GetDevice(), &allocInfo, &passBuffer));

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(passBuffer, &beginInfo));

		// The first command of the frame: the slot's timestamps were read back
		// before the fan-out. Only pass 0 of the frame's first graph claims it
		// (the graphs fan out one after another, so the flag is not shared).
		if (job.PassIndex == 0 && *job.FirstClaim)
		{
			job.Device->CmdResetTimestampPool(passBuffer, job.Slot);
			*job.FirstClaim = false;
		}

		// The list carries the job's slot (its buffer came from that slot's
		// pool): every record function reads it from the list.
		VulkanCommandList list(job.RhiDevice, passBuffer, job.Slot);
		job.Graph->RecordPass(job.PassIndex, *job.RhiDevice, list);

		VK_CHECK(vkEndCommandBuffer(passBuffer));
		*job.Out = { allocInfo.commandPool, passBuffer };
	}

	// The whole frame on the submit thread: the acquire, the record fan-out,
	// the submit and the present, strictly in order.
	void WindowPresenter::ProcessSubmittedFrame(FrameSubmission& frame)
	{
		HEARTH_ZONE("Frame Submit Thread - Process");
		HEARTH_ZONE("Frame - Prepare");

		uint32_t imageIndex = 0;
		VulkanSwapchain::AcquireResult acquireResult = VulkanSwapchain::AcquireResult::Success;
		if (m_Offscreen)
		{
			// No image to acquire: the acquire's fence wait becomes a wait for
			// the slot's previous submission, so the pool reset below
			// recycles that frame's buffers.
			const uint64_t previousValue = m_FrameLastValues[frame.FrameIndex].load();
			if (previousValue != 0)
				m_Device->WaitForTimelineValue(previousValue);
		}
		else
		{
			acquireResult = m_Swapchain.AcquireNextImage(frame.FrameIndex, imageIndex);
		}
		if (acquireResult == VulkanSwapchain::AcquireResult::OutOfDate ||
			acquireResult == VulkanSwapchain::AcquireResult::ShuttingDown)
		{
			// Out of date (a rebuild request is raised; the loop thread
			// performs it) or the surface is going away: the frame skips its
			// draw. The counters are still released, or the next slot-free
			// wait and the next submit park would never wake.
			if (frame.RecordDone != loom::kInvalidCounter)
				loom::Signal(frame.RecordDone);
			if (frame.Complete != loom::kInvalidCounter)
				loom::Signal(frame.Complete);
			return;
		}
		m_SubmitLists.clear();
		{
			HEARTH_ZONE("Frame - Record");

			// The acquire waited for this slot's previous submission, so its
			// pools can reset and its timestamps can be read back. Every
			// (slot, thread) pool resets up front (each worker allocates from
			// its own).
			for (uint32_t thread = 0; thread < m_Device->GetRecordThreadCount(); thread++)
				vkResetCommandPool(m_Device->GetDevice(), m_Device->GetCommandPool(frame.Slot, thread), 0);
			// The previous frame's per-pass buffers are freed so the driver
			// recycles them (the pool reset alone never does; the reset's
			// cost grew with the uptime).
			for (const auto& [pool, buffer] : frame.RetireCommandBuffers)
				vkFreeCommandBuffers(m_Device->GetDevice(), pool, 1, &buffer);
			frame.RetireCommandBuffers.clear();
			// The slot's record-start reset: retire entries tagged for it can
			// free.
			m_RhiDevice.NoteSlotReset(frame.Slot);
			m_Device->BeginProfilingFrame(frame.Slot, m_IsMainWindow);

			// The slot's bindless set is brought up to date here, once, before
			// the fan-out - never on a worker, where the update would race the
			// sibling workers' binds on the same set.
			m_RhiDevice.ApplyDescriptorLog(frame.Slot);

			// Each graph's passes record on loom workers, each into a command
			// buffer from its worker's (slot, thread) pool; the lists submit
			// in the compiled order. The submit thread waits BlockOnly.
			bool firstClaim = true;
			const std::function<void(rg::RenderGraph&)> recordGraph = [&](rg::RenderGraph& graph) {
				const uint32_t passCount = graph.GetCompiledPassCount();
				if (passCount == 0)
					return;

				m_GraphLists.resize(passCount);
				m_RecordJobs.resize(passCount);
				loom::CounterHandle counter = loom::CreateCounter((int)passCount);
				for (uint32_t i = 0; i < passCount; i++)
				{
					m_RecordJobs[i] = { &graph, i, m_Device, &m_RhiDevice, frame.Slot, &firstClaim, &m_GraphLists[i] };
#if defined(HEARTH_GPU_DEBUG)
					// The pass name rides into the task's zone, so a profiler's
					// thread timeline shows the named passes.
					const uint32_t nameId = m_InternName ? m_InternName(graph.GetCompiledPasses()[i].Name.c_str()) : 0;
					loom::Submit(loom::TaskDesc{ .fn = &WindowPresenter::RecordPassTask, .userData = &m_RecordJobs[i], .signals = counter,
						.nameId = nameId, .window = (uint8_t)m_WindowSlotBase });
#else
					loom::Submit(loom::TaskDesc{ .fn = &WindowPresenter::RecordPassTask, .userData = &m_RecordJobs[i], .signals = counter });
#endif
				}
				loom::WaitFor(counter, loom::Help::BlockOnly);
				loom::DestroyCounter(counter);

				for (const auto& [pool, passBuffer] : m_GraphLists)
				{
					frame.RetireCommandBuffers.push_back({ pool, passBuffer });
					m_SubmitLists.push_back(passBuffer);
				}
			};

#if defined(HEARTH_GPU_DEBUG)
			// The frame debugger's read is the loop thread's: the write holds
			// the sink's lock for this block only.
			GpuFrameDebugSink* debugSink = m_Device->TryGetGpuFrameDebugSink();
			if (m_IsMainWindow && debugSink)
			{
				GpuFrameDebugSink::Writer debug = debugSink->Write();
				GpuFrameDebugInfo& debugInfo = debug.Info;
				debugInfo.SwapchainImageIndex = imageIndex;
				switch (m_Swapchain.GetPresentMode())
				{
					case VK_PRESENT_MODE_IMMEDIATE_KHR: debugInfo.PresentMode = "IMMEDIATE"; break;
					case VK_PRESENT_MODE_MAILBOX_KHR: debugInfo.PresentMode = "MAILBOX"; break;
					case VK_PRESENT_MODE_FIFO_KHR: debugInfo.PresentMode = "FIFO"; break;
					case VK_PRESENT_MODE_FIFO_RELAXED_KHR: debugInfo.PresentMode = "FIFO_RELAXED"; break;
					default: debugInfo.PresentMode = "OTHER"; break;
				}
			}
#endif

			const WindowFrame windowFrame = MakeWindowFrame(frame.Slot);
			const VkExtent2D extent = m_Offscreen ? VkExtent2D{ m_OffscreenWidth, m_OffscreenHeight } : m_Swapchain.GetExtent();
			const FrameRecord record{ windowFrame, imageIndex, extent.width, extent.height, m_Offscreen, recordGraph };

			// The clients' own graphs first (a viewport's content must exist
			// before the swapchain's UI samples it), then the swapchain graph.
			for (FrameClient* client : m_Clients)
			{
				if (client)
					client->RecordTargets(record);
			}
			if (!m_Offscreen)
				RecordSwapchainGraph(record, recordGraph);

			// The clients clear what the record read; the slot-free signal
			// comes after, so the next update for this slot starts clean.
			for (FrameClient* client : m_Clients)
			{
				if (client)
					client->FrameRecorded(frame.Slot);
			}

			if (frame.RecordDone != loom::kInvalidCounter)
				loom::Signal(frame.RecordDone);
		}

		{
			HEARTH_ZONE("Frame - Submit");
			if (m_Offscreen)
			{
				// No present: the timeline signal only, under the queue lock
				// like the swapchain's SubmitAndPresent.
				VkSemaphore timelineSemaphore = m_Device->GetTimelineSemaphore();
				uint64_t timelineValue = 0;
				VkTimelineSemaphoreSubmitInfo timelineSubmit{};
				timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
				timelineSubmit.signalSemaphoreValueCount = 1;
				timelineSubmit.pSignalSemaphoreValues = &timelineValue;
				VkSubmitInfo submitInfo{};
				submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				submitInfo.pNext = &timelineSubmit;
				submitInfo.commandBufferCount = (uint32_t)m_SubmitLists.size();
				submitInfo.pCommandBuffers = m_SubmitLists.empty() ? nullptr : m_SubmitLists.data();
				submitInfo.signalSemaphoreCount = 1;
				submitInfo.pSignalSemaphores = &timelineSemaphore;
				{
					std::lock_guard queueLock(m_Device->GetQueueMutex());
					timelineValue = m_Device->AllocateTimelineValue();
					VK_CHECK(vkQueueSubmit(m_Device->GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
				}
				m_FrameLastValues[frame.FrameIndex].store(timelineValue);
			}
			else
			{
				m_Swapchain.SubmitAndPresent(frame.FrameIndex, imageIndex, m_SubmitLists);
			}
		}

		// The frame is done, the present included: the submit park wakes.
		if (frame.Complete != loom::kInvalidCounter)
			loom::Signal(frame.Complete);
	}

	void WindowPresenter::RecordSwapchainGraph(const FrameRecord& record, const std::function<void(rg::RenderGraph&)>& recordGraph)
	{
		rg::RenderGraph graph(m_TransientPool);
		// Imported in the acquired state, so the first barrier chains with
		// the acquire's wait.
		const rg::RGTexture image = graph.Import(m_Swapchain.GetImageHandle(record.ImageIndex), TextureState::SwapchainAcquired);
		SwapchainGraph swapchain(*this, graph, image, record.ImageIndex);

		auto& clearPass = graph.AddPass("Clear Swapchain");
		clearPass.Write(image, TextureState::RenderTarget);
		const uint32_t imageIndex = record.ImageIndex;
		clearPass.SetExecute([this, imageIndex](CommandList& list, const rg::PassResources&)
		{
			VulkanCommandList& vkList = static_cast<VulkanCommandList&>(list);
			// Each pass records into its own list, so each opens and closes
			// the rendering scope itself; this one clears.
			BeginSwapchainRendering(vkList, imageIndex, true);
			vkCmdEndRendering(vkList.GetCommandBuffer());
		});

		for (FrameClient* client : m_Clients)
		{
			if (client)
				client->RecordSwapchain(swapchain, record);
		}

		auto& presentPass = graph.AddPass("Present Transition");
		// Declaration-only: the graph emits the attachment -> present barrier
		// before this empty pass, after the last scope closed.
		presentPass.Write(image, TextureState::Present);

		graph.Compile();

#if defined(HEARTH_GPU_DEBUG)
		if (GpuFrameDebugSink* debugSink = m_Device->TryGetGpuFrameDebugSink())
		{
			GpuFrameDebugSink::Writer debug = debugSink->Write();
			GpuFrameDebugInfo& info = debug.Info;
			for (const auto& barrier : graph.GetCompiledBarriers())
			{
				GpuBarrierRecord barrierRecord;
				barrierRecord.Name = barrier.PassName;
				barrierRecord.OldLayout = barrier.FromName();
				barrierRecord.NewLayout = barrier.ToName();
				info.Barriers.push_back(barrierRecord);
			}
		}
#endif

		recordGraph(graph);

		// The final-state carry is single-threaded, after the fan-out (the
		// workers must not touch the pool).
		graph.RecordFinalStates();
		if (swapchain.HasDepth())
			m_DepthLayout = StateToLayout(graph.GetFinalState(swapchain.Depth()));

		for (FrameClient* client : m_Clients)
		{
			if (client)
				client->SwapchainRecorded(graph, record);
		}
	}

	void WindowPresenter::BeginSwapchainRendering(VulkanCommandList& list, uint32_t imageIndex, bool clear)
	{
		VkCommandBuffer commandBuffer = list.GetCommandBuffer();
		float clearColour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		for (FrameClient* client : m_Clients)
		{
			if (client && client->ClearColour(list.FrameSlot(), clearColour))
				break;
		}

		VkRenderingAttachmentInfo colourAttachment{};
		colourAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colourAttachment.imageView = m_Swapchain.GetImageView(imageIndex);
		colourAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colourAttachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		colourAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colourAttachment.clearValue.color.float32[0] = clearColour[0];
		colourAttachment.clearValue.color.float32[1] = clearColour[1];
		colourAttachment.clearValue.color.float32[2] = clearColour[2];
		colourAttachment.clearValue.color.float32[3] = clearColour[3];

		VkRenderingAttachmentInfo depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAttachment.imageView = m_Swapchain.GetDepthImageView();
		depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
		depthAttachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.clearValue.depthStencil.depth = 1.0f;

		VkRenderingInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea.offset = { 0, 0 };
		renderingInfo.renderArea.extent = m_Swapchain.GetExtent();
		renderingInfo.layerCount = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments = &colourAttachment;
		renderingInfo.pDepthAttachment = &depthAttachment;

		vkCmdBeginRendering(commandBuffer, &renderingInfo);
	}

	void WindowPresenter::WaitForSubmitIdle()
	{
		for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
		{
			if (m_FrameSubmissions[i].Complete != loom::kInvalidCounter)
				loom::WaitFor(m_FrameSubmissions[i].Complete, loom::Help::WhileWaiting);
		}
	}

	void WindowPresenter::WaitForRecordIdle()
	{
		// BlockOnly: the records belong to the loom workers; the loop thread
		// must not help them (they may be blocked on the very images this
		// wait precedes rebuilding).
		for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
		{
			if (m_FrameSubmissions[i].RecordDone != loom::kInvalidCounter)
				loom::WaitFor(m_FrameSubmissions[i].RecordDone, loom::Help::BlockOnly);
		}
	}

	// The every-window parks walk the device's window table, which only
	// presenters' construction and Stop write (on the loop thread).
	void WindowPresenter::WaitForAllRecordIdle(VulkanDevice& device)
	{
		for (VulkanWindowContext* window : device.GetLiveWindowContexts())
		{
			WindowPresenter* presenter = static_cast<WindowPresenter*>(window);
			if (!presenter)
				continue;
			HEARTH_ASSERT(presenter->IsLoopThread(), "the every-window record park runs on the loop thread only");
			presenter->WaitForRecordIdle();
		}
	}

	void WindowPresenter::WaitForAllSubmitIdle(VulkanDevice& device)
	{
		for (VulkanWindowContext* window : device.GetLiveWindowContexts())
		{
			WindowPresenter* presenter = static_cast<WindowPresenter*>(window);
			if (!presenter)
				continue;
			HEARTH_ASSERT(presenter->IsLoopThread(), "the every-window submit park runs on the loop thread only");
			presenter->WaitForSubmitIdle();
		}
	}

	void WindowPresenter::ParkSecondaryWindowsForShutdown(VulkanDevice& device)
	{
		for (VulkanWindowContext* window : device.GetLiveWindowContexts())
		{
			WindowPresenter* presenter = static_cast<WindowPresenter*>(window);
			// The main window is skipped: its own Stop raises the shutdown
			// flag first, so its acquire stays interruptible.
			if (!presenter || presenter->m_IsMainWindow)
				continue;
			HEARTH_ASSERT(presenter->IsLoopThread(), "the secondary-window shutdown park runs on the loop thread only");
			// The flag the join raises: an acquire that keeps timing out gives
			// up instead of wedging the teardown.
			if (!presenter->m_Offscreen)
				presenter->m_Swapchain.RequestShutdown();
			presenter->WaitForSubmitIdle();
		}
	}

} // namespace hearth::rhi
