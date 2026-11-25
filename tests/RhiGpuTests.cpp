// The GPU layer on a real device (Mesa's lavapipe in CI): an offscreen
// device with no surface, driven the way a program's window frame drives
// it - a window slot claimed, the descriptor log applied at each slot's
// record start, the retire queue drained at the frame's end.

#include "hearth/rhi/RenderGraph.h"
#include "hearth/rhi/vulkan/VulkanDevice.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"
#include "hearth/rhi/vulkan/VulkanWindowContext.h"

#include "hamr/test.h"

#include "shaders/rhi_selftest_fill.comp.h"

#include <span>
#include <vector>

namespace hearth::rhi_gpu_tests {

	using namespace hearth::rhi;

	// The program's window, as the device sees it: one window, the main one.
	class TestWindow final : public VulkanWindowContext
	{
	public:
		uint32_t Slot = 0;
		bool IsMainWindow() const override { return true; }
		uint32_t GetCurrentFrameSlot() const override { return Slot; }
	};

	// An offscreen device with one window slot, torn down in order.
	class Gpu
	{
	public:
		Gpu()
		{
			Rhi.SetPipelineCacheFile("");
			Rhi.EnsureDevice(nullptr);
			VulkanDevice& vulkan = *Rhi.GetVulkan();
			SlotBase = vulkan.AllocateWindowSlot(&Window);
			Rhi.NoteSlotExpected(SlotBase);
			vulkan.SetMainContext(&Window);
			BeginFrame(0);
		}

		~Gpu()
		{
			Rhi.WaitIdle();
			VulkanDevice& vulkan = *Rhi.GetVulkan();
			vulkan.ClearMainContext(&Window);
			vulkan.ReleaseWindowSlot(SlotBase);
			Rhi.ShutdownDevice();
		}

		// A frame's record start on the window's slot for `frame`: the slot's
		// reset is noted and its bindless set brought up to date.
		void BeginFrame(uint32_t frame)
		{
			Window.Slot = SlotBase + frame % VulkanDevice::FramesInFlight;
			Rhi.NoteSlotReset(Window.Slot);
			Rhi.ApplyDescriptorLog(Window.Slot);
		}

		// Records `record` into one submission and waits for it.
		template <typename Record>
		void Submit(Record record)
		{
			VulkanDevice& vulkan = *Rhi.GetVulkan();
			VkCommandBuffer commands = vulkan.BeginSingleTimeCommands();
			{
				VulkanCommandList list(&Rhi, commands, Window.Slot);
				record(list);
			}
			vulkan.EndSingleTimeCommands(commands);
		}

		VulkanRhiDevice Rhi;
		TestWindow Window;
		uint32_t SlotBase = 0;
	};

	constexpr uint32_t kCount = 1000;

	TEST_CASE("rhi gpu: a compute dispatch fills a storage buffer through the render graph, read back on the host")
	{
		Gpu gpu;
		VulkanRhiDevice& device = gpu.Rhi;

		const ShaderHandle shader = device.CreateShader(std::span<const uint32_t>(kRhiSelftestFillSpirv), ShaderStage::Compute, "rhi_selftest_fill.comp");
		ComputePipelineDesc desc;
		desc.ComputeShader = shader;
		desc.PushConstantSize = 2 * sizeof(uint32_t);
		const size_t cacheBefore = device.GetPipelineCacheSize();
		const PipelineHandle pipeline = device.CreateComputePipeline(desc, "fill");
		// The pipeline cache gives the same desc the same pipeline.
		const PipelineHandle again = device.CreateComputePipeline(desc, "fill (hit)");
		CHECK(again.Index == pipeline.Index);
		CHECK(again.Generation == pipeline.Generation);
		CHECK(device.GetPipelineCacheSize() == cacheBefore + 1);
		CHECK(device.GetPipelineBindPoint(pipeline) == VK_PIPELINE_BIND_POINT_COMPUTE);
		device.DestroyPipeline(again);

		BufferDesc bufferDesc;
		bufferDesc.Size = kCount * sizeof(uint32_t);
		bufferDesc.HostVisible = true;
		bufferDesc.Usage = BufferUsage::Storage;
		const BufferHandle buffer = device.CreateBuffer(bufferDesc, "fill target");
		const std::vector<uint32_t> zeros(kCount, 0u);
		device.SetBufferData(buffer, zeros.data(), bufferDesc.Size);
		const uint32_t bindlessIndex = device.AllocateBindlessBufferIndex();
		device.WriteBindlessBuffer(bindlessIndex, buffer);
		// The write reaches the slot's set at its next record start.
		gpu.BeginFrame(1);
		gpu.BeginFrame(2);

		// The dispatch writes the buffer; a declaration-only pass hands it
		// to the host (the readback's barrier).
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		const rg::RGBuffer target = graph.ImportBuffer(buffer, BufferState::Undefined);
		auto& fill = graph.AddPass("Fill");
		fill.Write(target, BufferState::ComputeWrite);
		fill.SetExecute([=](CommandList& list, const rg::PassResources&) {
			const uint32_t push[2] = { bindlessIndex, kCount };
			list.BindPipeline(pipeline);
			list.BindGlobalSet();
			list.PushConstants(kPushStageCompute, 0, push, sizeof(push));
			list.Dispatch((kCount + 63) / 64, 1, 1);
		});
		auto& readback = graph.AddPass("Readback");
		readback.Write(target, BufferState::HostRead);
		graph.Compile();
		bool computeToHost = false;
		for (const auto& barrier : graph.GetCompiledBarriers())
			computeToHost |= barrier.IsBuffer && barrier.BufferFrom == BufferState::ComputeWrite && barrier.BufferTo == BufferState::HostRead;
		CHECK(computeToHost);

		gpu.Submit([&](VulkanCommandList& list) { graph.Execute(device, list); });

		const uint32_t* values = static_cast<const uint32_t*>(device.GetBufferMappedData(buffer));
		REQUIRE(values != nullptr);
		uint32_t wrong = 0;
		for (uint32_t i = 0; i < kCount; i++)
			if (values[i] != i * 3u + 7u)
				wrong++;
		CHECK(wrong == 0u);

		device.DestroyPipeline(pipeline);
		device.DestroyShader(shader);
		device.DestroyBuffer(buffer);
	}

	TEST_CASE("rhi gpu: a destroyed texture's bindless index stays live until the retire queue frees it")
	{
		Gpu gpu;
		VulkanRhiDevice& device = gpu.Rhi;

		TextureDesc desc;
		desc.Width = 4;
		desc.Height = 4;
		const std::vector<uint8_t> pixels(4 * 4 * 4, 0x80);
		const TextureHandle texture = device.CreateTexture(desc, pixels.data(), pixels.size(), "retired");
		const uint32_t index = device.GetBindlessIndex(texture);
		CHECK(index != 0u); // 0 is the null texture
		CHECK(device.IsBindlessIndexLive(index));

		// An in-flight frame may still sample it: the destroy only queues it.
		device.DestroyTexture(texture);
		device.DrainRetireQueue();
		CHECK(device.IsBindlessIndexLive(index));

		// Frames go by: each slot's record start resets it and applies the
		// tombstone write, and the frame's end drains the queue.
		for (uint32_t frame = 3; frame < 11; frame++)
		{
			gpu.BeginFrame(frame);
			device.DrainRetireQueue();
		}
		CHECK_FALSE(device.IsBindlessIndexLive(index));
	}

} // namespace hearth::rhi_gpu_tests
