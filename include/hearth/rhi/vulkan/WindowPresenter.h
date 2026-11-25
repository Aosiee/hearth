#pragma once

// One window's frame on the GPU: its swapchain (none offscreen), its frame
// slots on the device, the dedicated submit thread that acquires, records,
// submits and presents, and the parks a program needs around them. What a
// frame draws comes from its FrameClients, called in registration order at
// fixed points of the frame.

#include "hearth/rhi/RenderGraph.h"
#include "hearth/rhi/vulkan/VulkanDevice.h"
#include "hearth/rhi/vulkan/VulkanSwapchain.h"
#include "hearth/rhi/vulkan/VulkanWindowContext.h"
#include "hearth/rhi/vulkan/VulkanWindowFrame.h"

#include "loom/loom.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace hearth::platform { class Window; }

namespace hearth::rhi {

	class VulkanCommandList;
	class VulkanRhiDevice;
	class WindowPresenter;

	// The loop thread's update of one frame: after any swapchain rebuild,
	// before the wait for the frame slot's previous record.
	struct FrameUpdate
	{
		WindowPresenter& Presenter;
		hearth::platform::Window* Window = nullptr; // null offscreen
		uint32_t Width = 0;                         // the swapchain extent, or the offscreen extent
		uint32_t Height = 0;
		uint32_t FrameSlot = 0;                     // the slot this frame publishes into
		uint32_t WindowIndex = 0;
		bool Offscreen = false;
	};

	// One frame's record on the submit thread.
	struct FrameRecord
	{
		const WindowFrame& Frame;
		uint32_t ImageIndex = 0; // the acquired swapchain image (0 offscreen)
		uint32_t Width = 0;
		uint32_t Height = 0;
		bool Offscreen = false;
		// Records a graph the client built: its passes fan out to the record
		// workers and their lists join this frame's submit, in call order.
		const std::function<void(rg::RenderGraph&)>& RecordGraph;
	};

	// The swapchain graph a windowed frame builds: the acquired image is
	// imported and cleared before the clients add their passes, and the
	// presenter ends it with the present transition.
	class SwapchainGraph
	{
	public:
		SwapchainGraph(WindowPresenter& presenter, rg::RenderGraph& graph, rg::RGTexture image, uint32_t imageIndex)
			: m_Presenter(presenter), m_Graph(graph), m_Image(image), m_ImageIndex(imageIndex) {}

		rg::RenderGraph& Graph() { return m_Graph; }
		rg::RGTexture Image() const { return m_Image; }
		// The swapchain depth, imported on first use (a client's own imports
		// before it keep their order in the graph).
		rg::RGTexture Depth();
		bool HasDepth() const { return m_DepthImported; }
		hearth::platform::Window* Window() const;
		VkExtent2D Extent() const;

		// Opens the swapchain's rendering scope in a pass's list (colour and
		// depth loaded; the first pass clears them); the pass ends it.
		void BeginRendering(VulkanCommandList& list, bool clear) const;

	private:
		WindowPresenter& m_Presenter;
		rg::RenderGraph& m_Graph;
		rg::RGTexture m_Image;
		uint32_t m_ImageIndex = 0;
		rg::RGTexture m_Depth{};
		bool m_DepthImported = false;
	};

	// What a program adds to every frame of a window. Every call but the
	// record ones runs on the loop thread.
	class FrameClient
	{
	public:
		virtual ~FrameClient() = default;

		// Asked first each frame: work that needs every window's GPU work
		// idle (a shader or code reload). When any client wants it, every
		// window's records and submits are parked, then Parked() runs on
		// each client that wanted it.
		virtual bool WantsPark() { return false; }
		virtual void Parked() {}

		// The frame's size is final here; the slot's previous record may
		// still be running.
		virtual void Update(const FrameUpdate&) {}
		// The slot's previous record is done: the frame's data goes into it.
		virtual void Publish(const WindowFrame&) {}

		// Submit thread: graphs of the client's own (offscreen targets).
		virtual void RecordTargets(const FrameRecord&) {}
		// Submit thread, windowed frames: passes into the swapchain graph.
		virtual void RecordSwapchain(SwapchainGraph&, const FrameRecord&) {}
		// Submit thread, after the swapchain graph recorded (its final
		// states are known).
		virtual void SwapchainRecorded(const rg::RenderGraph&, const FrameRecord&) {}
		// Submit thread, after every record of the frame, before the slot is
		// handed back to the loop thread.
		virtual void FrameRecorded(uint32_t frameSlot) { (void)frameSlot; }

		// The swapchain clear colour for a frame slot (any thread). The first
		// client that answers decides.
		virtual bool ClearColour(uint32_t frameSlot, float rgba[4]) { (void)frameSlot; (void)rgba; return false; }
	};

	struct WindowPresenterDesc
	{
		hearth::platform::Window* Window = nullptr; // null: offscreen (no surface, no present)
		bool IsMainWindow = false;
		uint32_t OffscreenWidth = 1280;             // the FrameUpdate size offscreen
		uint32_t OffscreenHeight = 720;
		// The program's own pointer (its per-window object), for the parks'
		// and the main window's callers.
		void* Owner = nullptr;
		// Names for the profiler's task zones (optional; 0 = unnamed).
		std::function<uint32_t(const char*)> InternName;
	};

	class WindowPresenter final : public VulkanWindowContext
	{
	public:
		// Claims a window slot on the device and starts the submit thread.
		// Loop thread (the thread that created the device).
		WindowPresenter(VulkanRhiDevice& device, const WindowPresenterDesc& desc);
		~WindowPresenter();
		WindowPresenter(const WindowPresenter&) = delete;
		WindowPresenter& operator=(const WindowPresenter&) = delete;

		// The clients, in call order (replaced whole; loop thread, between
		// frames).
		void SetClients(std::span<FrameClient* const> clients);

		// The window's frame: park, rebuild, update, wait for the slot,
		// publish, hand the frame to the submit thread, drain the retire
		// queue. Loop thread.
		void Present();

		// Stops the submit thread and waits for the window's last frame.
		// Called before the program destroys anything the clients' records
		// read; the destructor calls it too.
		void Stop();

		void SetVsync(bool enabled) { m_Swapchain.SetVsync(enabled); }

		// Blocks until every frame in flight is submitted (the present
		// included) / until every frame in flight finished its record.
		void WaitForSubmitIdle();
		void WaitForRecordIdle();
		// The same over every live presenter on the device. Loop thread.
		static void WaitForAllRecordIdle(VulkanDevice& device);
		static void WaitForAllSubmitIdle(VulkanDevice& device);
		// The teardown park: every live secondary window stops acquiring and
		// finishes its frames. Loop thread, after the frame loop ended.
		static void ParkSecondaryWindowsForShutdown(VulkanDevice& device);

		// VulkanWindowContext
		bool IsMainWindow() const override { return m_IsMainWindow; }
		uint32_t GetCurrentFrameSlot() const override;

		uint32_t GetPreviousFrameSlot() const;
		uint64_t GetFrameSubmitValueForSlot(uint32_t slot) const;
		uint32_t GetWindowSlotBase() const { return m_WindowSlotBase; }
		uint32_t GetWindowIndex() const { return m_WindowSlotBase / VulkanDevice::FramesInFlight; }
		bool IsOffscreen() const { return m_Offscreen; }
		bool IsLoopThread() const { return m_Device->IsGameThread(); }
		void* GetOwner() const { return m_Owner; }
		VulkanSwapchain& GetSwapchain() { return m_Swapchain; }
		VulkanRhiDevice& GetRhiDevice() { return m_RhiDevice; }
		hearth::platform::Window* GetWindow() const { return m_Window; }
		WindowFrame MakeWindowFrame(uint32_t slot) const;

	private:
		friend class SwapchainGraph;

		struct FrameSubmission
		{
			WindowPresenter* Presenter = nullptr;
			uint32_t Slot = 0;
			uint32_t FrameIndex = 0;
			// Raised by the enqueue, lowered after the record: the next
			// frame on this slot waits it (the slot-free condition).
			loom::CounterHandle RecordDone = loom::kInvalidCounter;
			// Raised by the enqueue, lowered after the present.
			loom::CounterHandle Complete = loom::kInvalidCounter;
			// The previous frame's per-pass buffers, freed at the slot's next
			// reset (the pool reset alone never recycles them).
			std::vector<std::pair<VkCommandPool, VkCommandBuffer>> RetireCommandBuffers;
		};
		struct RecordJob
		{
			rg::RenderGraph* Graph = nullptr;
			uint32_t PassIndex = 0;
			VulkanDevice* Device = nullptr;
			VulkanRhiDevice* RhiDevice = nullptr;
			uint32_t Slot = 0;
			bool* FirstClaim = nullptr;
			std::pair<VkCommandPool, VkCommandBuffer>* Out = nullptr;
		};

		static void FrameSubmitTask(void* userData);
		static void RecordPassTask(void* userData);
		void ProcessSubmittedFrame(FrameSubmission& frame);
		void ParkForClients();
		void WaitForSlot();
		void Enqueue();
		void RecordSwapchainGraph(const FrameRecord& record, const std::function<void(rg::RenderGraph&)>& recordGraph);
		void BeginSwapchainRendering(VulkanCommandList& list, uint32_t imageIndex, bool clear);
		rg::RGTexture ImportDepth(rg::RenderGraph& graph);
		void EnsureDepthHandle();

		VulkanRhiDevice& m_RhiDevice;
		VulkanDevice* m_Device = nullptr;
		hearth::platform::Window* m_Window = nullptr;
		void* m_Owner = nullptr;
		std::function<uint32_t(const char*)> m_InternName;
		const bool m_IsMainWindow;
		const bool m_Offscreen;
		uint32_t m_OffscreenWidth = 0;
		uint32_t m_OffscreenHeight = 0;
		bool m_Stopped = false;

		VulkanSwapchain m_Swapchain;
		uint32_t m_WindowSlotBase = 0;
		uint32_t m_CurrentFrame = 0;
		std::vector<FrameClient*> m_Clients;

		rg::TransientTexturePool m_TransientPool;
		// The swapchain depth as a graph-tracked resource (re-adopted when
		// the swapchain recreates it), its layout carried across frames.
		TextureHandle m_DepthHandle;
		VkImageLayout m_DepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		uint64_t m_AdoptedDepthGeneration = 0;

		loom::DedicatedId m_SubmitThread = loom::kInvalidDedicated;
		FrameSubmission m_FrameSubmissions[VulkanDevice::FramesInFlight];
		// The record fan-out's scratch (the submit thread runs one frame at a
		// time).
		std::vector<std::pair<VkCommandPool, VkCommandBuffer>> m_GraphLists;
		std::vector<RecordJob> m_RecordJobs;
		std::vector<VkCommandBuffer> m_SubmitLists;
		// Offscreen frames' submit values (the swapchain keeps them when
		// there is one).
		std::atomic<uint64_t> m_FrameLastValues[VulkanDevice::FramesInFlight] = {};
	};

} // namespace hearth::rhi
