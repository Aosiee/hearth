// WindowPresenter on a real device (Mesa's lavapipe in CI), offscreen: the
// clients' calls and their order, the frame slots, the park, and a client's
// graph recorded into the frame's submit.

#include "hearth/rhi/RenderGraph.h"
#include "hearth/rhi/vulkan/VulkanDevice.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"
#include "hearth/rhi/vulkan/WindowPresenter.h"

#include "hamr/test.h"
#include "loom/loom.h"

#include "shaders/rhi_selftest_fill.comp.h"

#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace hearth::presenter_gpu_tests {

	using namespace hearth::rhi;

	// The pool the presenters' threads run on, one for the binary: loom's
	// thread indices are process-wide, and the device keys its record
	// command pools on them, so every case's workers must be the same ones.
	void EnsurePool()
	{
		static loom::Pool pool({ .workerCount = 4 });
		(void)pool;
	}

	// An offscreen device on its own (the presenter claims the window slot
	// and the main context).
	class Device
	{
	public:
		Device()
		{
			EnsurePool();
			Rhi.SetPipelineCacheFile("");
			Rhi.EnsureDevice(nullptr);
		}
		~Device()
		{
			Rhi.WaitIdle();
			Rhi.ShutdownDevice();
		}

		VulkanRhiDevice Rhi;
	};

	struct Call
	{
		std::string What;
		uint32_t Slot = 0;
	};

	// Logs every call (the record ones arrive on the submit thread).
	class LoggingClient : public FrameClient
	{
	public:
		bool WantsPark() override
		{
			Log("wants-park", 0);
			return ParkOnFrame == m_Frames;
		}
		void Parked() override { Log("parked", 0); }
		void Update(const FrameUpdate& update) override
		{
			m_Frames++;
			LastUpdate.Width = update.Width;
			LastUpdate.Height = update.Height;
			LastUpdate.Window = update.Window;
			LastUpdate.Offscreen = update.Offscreen;
			Log("update", update.FrameSlot);
		}
		void Publish(const WindowFrame& frame) override { Log("publish", frame.Slot); }
		void RecordTargets(const FrameRecord& record) override { Log("record-targets", record.Frame.Slot); }
		void RecordSwapchain(SwapchainGraph&, const FrameRecord& record) override { Log("record-swapchain", record.Frame.Slot); }
		void FrameRecorded(uint32_t frameSlot) override { Log("recorded", frameSlot); }

		std::vector<Call> Calls()
		{
			std::lock_guard lock(m_Mutex);
			return m_Calls;
		}

		struct
		{
			uint32_t Width = 0, Height = 0;
			hearth::platform::Window* Window = nullptr;
			bool Offscreen = false;
		} LastUpdate;
		uint32_t ParkOnFrame = ~0u;

	private:
		void Log(const char* what, uint32_t slot)
		{
			std::lock_guard lock(m_Mutex);
			m_Calls.push_back({ what, slot });
		}

		std::mutex m_Mutex;
		std::vector<Call> m_Calls;
		uint32_t m_Frames = 0;
	};

	std::vector<Call> Only(const std::vector<Call>& calls, const std::string& what)
	{
		std::vector<Call> out;
		for (const Call& call : calls)
			if (call.What == what)
				out.push_back(call);
		return out;
	}

	TEST_CASE("presenter gpu: an offscreen presenter calls its clients in order, frame after frame")
	{
		Device device;
		LoggingClient client;
		constexpr uint32_t kFrames = 5;
		uint32_t slotBase = 0;
		{
			WindowPresenterDesc desc;
			desc.IsMainWindow = true;
			desc.OffscreenWidth = 64;
			desc.OffscreenHeight = 32;
			WindowPresenter presenter(device.Rhi, desc);
			CHECK(presenter.IsOffscreen());
			CHECK(&device.Rhi.GetVulkan()->GetMainContext() == static_cast<VulkanWindowContext*>(&presenter));
			slotBase = presenter.GetWindowSlotBase();

			FrameClient* clients[] = { &client };
			presenter.SetClients(clients);
			for (uint32_t frame = 0; frame < kFrames; frame++)
				presenter.Present();
			presenter.Stop();
		}
		// The main window's presenter leaves the main context on the way out.
		CHECK(device.Rhi.GetVulkan()->TryGetMainContext() == nullptr);

		CHECK(client.LastUpdate.Width == 64u);
		CHECK(client.LastUpdate.Height == 32u);
		CHECK(client.LastUpdate.Window == nullptr);
		CHECK(client.LastUpdate.Offscreen);

		const std::vector<Call> calls = client.Calls();
		const std::vector<Call> updates = Only(calls, "update");
		const std::vector<Call> publishes = Only(calls, "publish");
		const std::vector<Call> records = Only(calls, "record-targets");
		const std::vector<Call> recorded = Only(calls, "recorded");
		REQUIRE(updates.size() == kFrames);
		REQUIRE(publishes.size() == kFrames);
		REQUIRE(records.size() == kFrames);
		REQUIRE(recorded.size() == kFrames);
		CHECK(Only(calls, "record-swapchain").empty()); // offscreen: no swapchain graph
		CHECK(Only(calls, "parked").empty());
		for (uint32_t frame = 0; frame < kFrames; frame++)
		{
			const uint32_t slot = slotBase + frame % VulkanDevice::FramesInFlight;
			CAPTURE(frame);
			CHECK(updates[frame].Slot == slot);
			CHECK(publishes[frame].Slot == slot);
			CHECK(records[frame].Slot == slot);
			CHECK(recorded[frame].Slot == slot);
		}

		// Each frame's loop calls in order, and each frame's record ends
		// before its slot is used again.
		uint32_t frame = 0;
		std::vector<std::string> loop;
		for (const Call& call : calls)
			if (call.What == "wants-park" || call.What == "update" || call.What == "publish")
				loop.push_back(call.What);
		REQUIRE(loop.size() == kFrames * 3);
		for (frame = 0; frame < kFrames; frame++)
		{
			CHECK(loop[frame * 3 + 0] == "wants-park");
			CHECK(loop[frame * 3 + 1] == "update");
			CHECK(loop[frame * 3 + 2] == "publish");
		}
		std::vector<std::string> submit;
		for (const Call& call : calls)
			if (call.What == "record-targets" || call.What == "recorded")
				submit.push_back(call.What);
		REQUIRE(submit.size() == kFrames * 2);
		for (frame = 0; frame < kFrames; frame++)
		{
			CHECK(submit[frame * 2 + 0] == "record-targets");
			CHECK(submit[frame * 2 + 1] == "recorded");
		}
	}

	TEST_CASE("presenter gpu: a client that wants the park gets it before the frame's update")
	{
		Device device;
		LoggingClient client;
		client.ParkOnFrame = 2;
		{
			WindowPresenterDesc desc;
			desc.IsMainWindow = true;
			WindowPresenter presenter(device.Rhi, desc);
			FrameClient* clients[] = { &client };
			presenter.SetClients(clients);
			for (uint32_t frame = 0; frame < 4; frame++)
				presenter.Present();
		}
		std::vector<std::string> loop;
		for (const Call& call : client.Calls())
			if (call.What != "record-targets" && call.What != "recorded")
				loop.push_back(call.What);
		// Frame 2 (the third): wants-park, parked, update, publish.
		REQUIRE(loop.size() == 13u);
		CHECK(loop[6] == "wants-park");
		CHECK(loop[7] == "parked");
		CHECK(loop[8] == "update");
		CHECK(Only(client.Calls(), "parked").size() == 1u);
	}

	constexpr uint32_t kCount = 256;

	// Fills a buffer from a compute pass in every frame's target record.
	class FillClient final : public FrameClient
	{
	public:
		FillClient(PipelineHandle pipeline, BufferHandle buffer, uint32_t bindlessIndex)
			: m_Pipeline(pipeline), m_Buffer(buffer), m_BindlessIndex(bindlessIndex) {}

		void RecordTargets(const FrameRecord& record) override
		{
			rg::RenderGraph graph(m_Pool);
			const rg::RGBuffer target = graph.ImportBuffer(m_Buffer, BufferState::Undefined);
			auto& fill = graph.AddPass("Fill");
			fill.Write(target, BufferState::ComputeWrite);
			fill.SetExecute([pipeline = m_Pipeline, index = m_BindlessIndex](CommandList& list, const rg::PassResources&) {
				const uint32_t push[2] = { index, kCount };
				list.BindPipeline(pipeline);
				list.BindGlobalSet();
				list.PushConstants(kPushStageCompute, 0, push, sizeof(push));
				list.Dispatch((kCount + 63) / 64, 1, 1);
			});
			auto& readback = graph.AddPass("Readback");
			readback.Write(target, BufferState::HostRead);
			graph.Compile();
			record.RecordGraph(graph);
			Records++;
		}

		uint32_t Records = 0;

	private:
		rg::TransientTexturePool m_Pool;
		PipelineHandle m_Pipeline;
		BufferHandle m_Buffer;
		uint32_t m_BindlessIndex = 0;
	};

	TEST_CASE("presenter gpu: a client's graph is recorded into the frame's submit")
	{
		Device device;
		VulkanRhiDevice& rhi = device.Rhi;
		const ShaderHandle shader = rhi.CreateShader(std::span<const uint32_t>(kRhiSelftestFillSpirv), ShaderStage::Compute, "rhi_selftest_fill.comp");
		ComputePipelineDesc pipelineDesc;
		pipelineDesc.ComputeShader = shader;
		pipelineDesc.PushConstantSize = 2 * sizeof(uint32_t);
		const PipelineHandle pipeline = rhi.CreateComputePipeline(pipelineDesc, "fill");

		BufferDesc bufferDesc;
		bufferDesc.Size = kCount * sizeof(uint32_t);
		bufferDesc.HostVisible = true;
		bufferDesc.Usage = BufferUsage::Storage;
		const BufferHandle buffer = rhi.CreateBuffer(bufferDesc, "fill target");
		const std::vector<uint32_t> zeros(kCount, 0u);
		rhi.SetBufferData(buffer, zeros.data(), bufferDesc.Size);
		const uint32_t bindlessIndex = rhi.AllocateBindlessBufferIndex();
		rhi.WriteBindlessBuffer(bindlessIndex, buffer);

		FillClient client(pipeline, buffer, bindlessIndex);
		{
			WindowPresenterDesc desc;
			desc.IsMainWindow = true;
			WindowPresenter presenter(rhi, desc);
			FrameClient* clients[] = { &client };
			presenter.SetClients(clients);
			presenter.Present();
			presenter.Present();
			// Stop waits for the last frame's submit; the GPU then finishes it.
			presenter.Stop();
			rhi.WaitIdle();
		}
		CHECK(client.Records == 2u);

		const uint32_t* values = static_cast<const uint32_t*>(rhi.GetBufferMappedData(buffer));
		REQUIRE(values != nullptr);
		uint32_t wrong = 0;
		for (uint32_t i = 0; i < kCount; i++)
			if (values[i] != i * 3u + 7u)
				wrong++;
		CHECK(wrong == 0u);

		rhi.DestroyPipeline(pipeline);
		rhi.DestroyShader(shader);
		rhi.DestroyBuffer(buffer);
	}

} // namespace hearth::presenter_gpu_tests
