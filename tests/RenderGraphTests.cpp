#pragma once

// The RenderGraph core suite: topological ordering, culling, barrier
// derivation, transient-pool reuse, and the Execute barrier emission,
// against mock Device/CommandList (the core is pure data-flow - no GPU).

#include "TinyEngine/RenderGraph/RenderGraph.h"

#include "loom.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <string>

namespace TinyEngine {
namespace rg_tests {

	// ---- Mocks ------------------------------------------------------------------

	class MockDevice final : public rhi::Device
	{
	public:
		uint32_t TextureCreates = 0;

		rhi::BufferHandle CreateBuffer(const rhi::BufferDesc&, const char*) override { return {}; }
		void DestroyBuffer(rhi::BufferHandle) override {}
		void SetBufferData(rhi::BufferHandle, const void*, uint64_t, uint64_t) override {}
		void UploadBufferViaStaging(rhi::BufferHandle, const void*, uint64_t) override {}
		void WriteBindlessBuffer(uint32_t, rhi::BufferHandle) override {}

		rhi::TextureHandle CreateTexture(const rhi::TextureDesc&, const void*, uint64_t, const char*) override
		{
			TextureCreates++;
			rhi::TextureHandle handle;
			handle.Index = TextureCreates; // distinct per call, never ~0u
			return handle;
		}
		rhi::TextureHandle CreateTextureFromBakedMips(uint32_t, uint32_t, const std::vector<std::vector<uint8_t>>&, const char*) override
		{
			TextureCreates++;
			rhi::TextureHandle handle;
			handle.Index = TextureCreates;
			return handle;
		}
		rhi::TextureHandle CreateTextureFromNativeImage(void*, uint32_t, uint32_t, rhi::Format) override
		{
			TextureCreates++;
			rhi::TextureHandle handle;
			handle.Index = TextureCreates;
			return handle;
		}
		rhi::TextureHandle AdoptExternalImage(void*) override
		{
			rhi::TextureHandle handle;
			handle.Index = 100; // a stable adopted handle
			return handle;
		}
		void DestroyTexture(rhi::TextureHandle) override {}
		void SetTextureData(rhi::TextureHandle, const void*, uint64_t) override {}
		void UpdateTextureBakedMips(rhi::TextureHandle, const std::vector<std::vector<uint8_t>>&) override {}
		uint32_t GetBindlessIndex(rhi::TextureHandle) override { return 0; }

		rhi::SamplerHandle CreateSampler(const rhi::SamplerDesc&, const char*) override { return {}; }
		void DestroySampler(rhi::SamplerHandle) override {}
		rhi::ShaderHandle CreateShader(std::span<const uint32_t>, rhi::ShaderStage, const char*, const rhi::ShaderIdentity* = nullptr) override { return {}; }
		void DestroyShader(rhi::ShaderHandle) override {}
		void InvalidatePipelines(const AssetID&) override {}
		rhi::PipelineHandle CreatePipeline(const rhi::PipelineStateDesc&, const char*) override { return {}; }
		void DestroyPipeline(rhi::PipelineHandle) override {}

		rhi::CommandListHandle CreateCommandList(rhi::QueueHandle, uint32_t) override { return {}; }
		void DestroyCommandList(rhi::CommandListHandle) override {}
		rhi::CommandList* GetCommandList(rhi::CommandListHandle) override { return nullptr; }
		rhi::CommandList* AdoptCommandBuffer(void*) override { return nullptr; }

		rhi::QueueHandle GetQueue(rhi::QueueKind) override { return {}; }
		void BeginUploadBatch() override {}
		void EndUploadBatch() override {}
		uint64_t Submit(rhi::QueueHandle, std::span<const rhi::CommandListHandle>,
			std::span<const rhi::TimelineWait>, std::span<const rhi::TimelineSignal>) override { return 0; }
		void WaitIdle() override {}

		rhi::SwapchainHandle CreateSwapchain(void*) override { return {}; }
		void DestroySwapchain(rhi::SwapchainHandle) override {}
	};

	class MockCommandList final : public rhi::CommandList
	{
	public:
		std::vector<std::pair<rhi::TextureState, rhi::TextureState>> Barriers;

		void Begin() override {}
		void End() override {}
		void BeginRendering(std::span<const rhi::AttachmentDesc>) override {}
		void EndRendering() override {}
		void SetViewport(float, float, float, float, float, float) override {}
		void SetScissor(int32_t, int32_t, uint32_t, uint32_t) override {}
		void SetDepthBias(float, float, float) override {}

		void Barrier(std::span<const rhi::BarrierDesc> barriers) override
		{
			for (const rhi::BarrierDesc& barrier : barriers)
			{
				for (uint32_t i = 0; i < barrier.TextureCount; i++)
					Barriers.push_back({ barrier.Textures[i].From, barrier.Textures[i].To });
			}
		}

		void BindPipeline(rhi::PipelineHandle) override {}
		void BindGlobalSet() override {}
		void PushConstants(uint32_t, uint32_t, const void*, uint32_t) override {}
		void BindVertexBuffer(rhi::BufferHandle, uint64_t) override {}
		void BindIndexBuffer(rhi::BufferHandle, uint64_t) override {}
		void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
		void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
		void Dispatch(uint32_t, uint32_t, uint32_t) override {}
		void CopyBufferToBuffer(rhi::BufferHandle, rhi::BufferHandle, uint64_t, uint64_t, uint64_t) override {}
		void CopyBufferToTexture(rhi::BufferHandle, rhi::TextureHandle, uint32_t, uint32_t) override {}
		void CopyTextureToBuffer(rhi::TextureHandle, rhi::BufferHandle, uint32_t, uint32_t) override {}
		void BeginLabel(const char*) override {}
		void EndLabel() override {}
		void WriteTimestamp(uint32_t) override {}
	};

	namespace {

		std::string NameAt(const rg::RenderGraph& graph, size_t index)
		{
			return graph.GetCompiledPasses()[index].Name;
		}

		rhi::TextureHandle ImportHandle(uint32_t index)
		{
			rhi::TextureHandle handle;
			handle.Index = index;
			return handle;
		}

		rg::RenderGraph MakeGraph(rg::TransientTexturePool& pool)
		{
			return rg::RenderGraph(pool);
		}

	} // namespace

	TEST_CASE("rendergraph: a reader is ordered after its writer")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture target = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture scene = graph.CreateTexture({});

		auto& scenePass = graph.AddPass("Scene");
		scenePass.Write(scene, rhi::TextureState::RenderTarget);
		scenePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& presentPass = graph.AddPass("Present");
		presentPass.Read(scene, rhi::TextureState::ShaderRead);
		presentPass.Write(target, rhi::TextureState::Present);
		presentPass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		REQUIRE(graph.GetCompiledPasses().size() == 2);
		CHECK(NameAt(graph, 0) == "Scene");
		CHECK(NameAt(graph, 1) == "Present");
	}

	TEST_CASE("rendergraph: independent passes keep registration order")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture a = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture b = graph.Import(ImportHandle(2), rhi::TextureState::Undefined);

		auto& first = graph.AddPass("First");
		first.Write(a, rhi::TextureState::Present);
		first.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& second = graph.AddPass("Second");
		second.Write(b, rhi::TextureState::Present);
		second.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		REQUIRE(graph.GetCompiledPasses().size() == 2);
		CHECK(NameAt(graph, 0) == "First");
		CHECK(NameAt(graph, 1) == "Second");
	}

	TEST_CASE("rendergraph: culling drops passes whose outputs are never consumed")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture orphanA = graph.CreateTexture({});
		rg::RGTexture orphanB = graph.CreateTexture({});

		auto& deadProducer = graph.AddPass("DeadProducer");
		deadProducer.Write(orphanA, rhi::TextureState::RenderTarget);
		deadProducer.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& deadConsumer = graph.AddPass("DeadConsumer");
		deadConsumer.Read(orphanA, rhi::TextureState::ShaderRead);
		deadConsumer.Write(orphanB, rhi::TextureState::RenderTarget);
		deadConsumer.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& live = graph.AddPass("Live");
		live.Write(exported, rhi::TextureState::Present);
		live.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		REQUIRE(graph.GetCompiledPasses().size() == 1);
		CHECK(NameAt(graph, 0) == "Live");
	}

	TEST_CASE("rendergraph: culling keeps a producer chain that feeds an export")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture intermediate = graph.CreateTexture({});

		auto& producer = graph.AddPass("Producer");
		producer.Write(intermediate, rhi::TextureState::RenderTarget);
		producer.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& consumer = graph.AddPass("Consumer");
		consumer.Read(intermediate, rhi::TextureState::ShaderRead);
		consumer.Write(exported, rhi::TextureState::Present);
		consumer.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		REQUIRE(graph.GetCompiledPasses().size() == 2);
		CHECK(NameAt(graph, 0) == "Producer");
		CHECK(NameAt(graph, 1) == "Consumer");
	}

	TEST_CASE("rendergraph: barrier derivation between consecutive users")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture scene = graph.CreateTexture({});

		auto& scenePass = graph.AddPass("Scene");
		scenePass.Write(scene, rhi::TextureState::RenderTarget);
		scenePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& samplePass = graph.AddPass("Sample");
		samplePass.Read(scene, rhi::TextureState::ShaderRead);
		samplePass.Write(exported, rhi::TextureState::Present);
		samplePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		// Three edges: the transient's first use (Undefined -> RenderTarget),
		// the state change (RenderTarget -> ShaderRead), and the export
		// (Undefined -> Present). Assert the specific edge that matters.
		bool found = false;
		for (const auto& barrier : graph.GetCompiledBarriers())
		{
			if (barrier.From == rhi::TextureState::RenderTarget && barrier.To == rhi::TextureState::ShaderRead)
			{
				found = true;
				CHECK(barrier.PassName == "Sample");
			}
		}
		CHECK(found);
	}

	TEST_CASE("rendergraph: same-state neighbours need no barrier")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture scene = graph.CreateTexture({});

		auto& scenePass = graph.AddPass("Scene");
		scenePass.Write(scene, rhi::TextureState::RenderTarget);
		scenePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& moreScene = graph.AddPass("MoreScene");
		moreScene.Write(scene, rhi::TextureState::RenderTarget);
		moreScene.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& present = graph.AddPass("Present");
		present.Read(scene, rhi::TextureState::ShaderRead);
		present.Write(exported, rhi::TextureState::Present);
		present.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		// The two adjacent RenderTarget writes must NOT produce a
		// RenderTarget -> RenderTarget edge; the RenderTarget -> ShaderRead
		// edge and the first-use / export edges are the only ones.
		for (const auto& barrier : graph.GetCompiledBarriers())
		{
			bool sameStateEdge = (barrier.From == rhi::TextureState::RenderTarget) && (barrier.To == rhi::TextureState::RenderTarget);
			CHECK_FALSE(sameStateEdge);
		}

		bool found = false;
		for (const auto& barrier : graph.GetCompiledBarriers())
		{
			if (barrier.From == rhi::TextureState::RenderTarget && barrier.To == rhi::TextureState::ShaderRead)
			{
				found = true;
				CHECK(barrier.PassName == "Present");
			}
		}
		CHECK(found);
	}

	TEST_CASE("rendergraph: an import's current state seeds the first barrier")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		// The external world left the image in ShaderRead; the first pass
		// renders into it.
		rg::RGTexture target = graph.Import(ImportHandle(1), rhi::TextureState::ShaderRead);

		auto& render = graph.AddPass("Render");
		render.Write(target, rhi::TextureState::RenderTarget);
		render.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		REQUIRE(graph.GetCompiledBarriers().size() == 1);
		CHECK(graph.GetCompiledBarriers()[0].From == rhi::TextureState::ShaderRead);
		CHECK(graph.GetCompiledBarriers()[0].To == rhi::TextureState::RenderTarget);
	}

	TEST_CASE("rendergraph: two live transients never alias one physical texture")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);

		rhi::TextureDesc desc;
		desc.Width = 800;
		desc.Height = 600;
		desc.RenderTarget = true;
		rg::RGTexture viewA = graph.CreateTexture(desc);
		rg::RGTexture viewB = graph.CreateTexture(desc);

		auto& viewAPass = graph.AddPass("ViewA");
		viewAPass.Write(viewA, rhi::TextureState::RenderTarget);
		viewAPass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& viewBPass = graph.AddPass("ViewB");
		viewBPass.Write(viewB, rhi::TextureState::RenderTarget);
		viewBPass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& present = graph.AddPass("Present");
		present.Read(viewA, rhi::TextureState::ShaderRead);
		present.Read(viewB, rhi::TextureState::ShaderRead);
		present.Write(exported, rhi::TextureState::Present);
		present.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		MockDevice device;
		MockCommandList commandList;
		graph.Execute(device, commandList, 0u);

		// Two live virtuals with the same desc: two physical textures.
		CHECK(device.TextureCreates == 2);
	}

	TEST_CASE("rendergraph: the pool reuses a transient across frames and carries its state")
	{
		rg::TransientTexturePool pool;
		MockDevice device;
		MockCommandList commandList;

		// Round one: the scene is rendered and sampled.
		{
			rg::RenderGraph graph(pool);
			rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
			rg::RGTexture scene = graph.CreateTexture({});

			auto& scenePass = graph.AddPass("Scene");
			scenePass.Write(scene, rhi::TextureState::RenderTarget);
			scenePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

			auto& sample = graph.AddPass("Sample");
			sample.Read(scene, rhi::TextureState::ShaderRead);
			sample.Write(exported, rhi::TextureState::Present);
			sample.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

			graph.Compile();
			graph.Execute(device, commandList, 0u);
		}

		// Round two: the same virtual index resolves to the same physical
		// texture, and the derivation seeds from ShaderRead (the state the
		// last frame left) - so the next write emits ShaderRead -> RenderTarget,
		// not Undefined -> RenderTarget.
		{
			rg::RenderGraph graph(pool);
			rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
			rg::RGTexture scene = graph.CreateTexture({});

			auto& scenePass = graph.AddPass("Scene");
			scenePass.Write(scene, rhi::TextureState::RenderTarget);
			scenePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

			auto& sample = graph.AddPass("Sample");
			sample.Read(scene, rhi::TextureState::ShaderRead);
			sample.Write(exported, rhi::TextureState::Present);
			sample.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

			graph.Compile();
			graph.Execute(device, commandList, 0u);

			// No new allocation for the scene transient in round two.
			CHECK(device.TextureCreates == 1);

			bool seeded = false;
			for (const auto& barrier : graph.GetCompiledBarriers())
				seeded |= (barrier.From == rhi::TextureState::ShaderRead && barrier.To == rhi::TextureState::RenderTarget);
			CHECK(seeded);
		}
	}

	TEST_CASE("rendergraph: execute emits the compiled barriers in pass order")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture scene = graph.CreateTexture({});

		auto& scenePass = graph.AddPass("Scene");
		scenePass.Write(scene, rhi::TextureState::RenderTarget);
		scenePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& sample = graph.AddPass("Sample");
		sample.Read(scene, rhi::TextureState::ShaderRead);
		sample.Write(exported, rhi::TextureState::Present);
		sample.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		MockDevice device;
		MockCommandList commandList;
		graph.Execute(device, commandList, 0u);

		// The RenderTarget -> ShaderRead edge must be emitted (alongside the
		// first-use and export edges).
		bool found = false;
		for (const auto& barrier : commandList.Barriers)
			found |= (barrier.first == rhi::TextureState::RenderTarget && barrier.second == rhi::TextureState::ShaderRead);
		CHECK(found);
	}

	TEST_CASE("rendergraph: passes resolve their own resource handles")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(7), rhi::TextureState::Undefined);
		rg::RGTexture scene = graph.CreateTexture({});

		rhi::TextureHandle seenImported;
		seenImported.Index = ~0u;

		auto& scenePass = graph.AddPass("Scene");
		scenePass.Write(scene, rhi::TextureState::RenderTarget);
		scenePass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		auto& present = graph.AddPass("Present");
		present.Read(scene, rhi::TextureState::ShaderRead);
		present.Write(exported, rhi::TextureState::Present);
		present.SetExecute([&](rhi::CommandList&, const rg::PassResources& resources)
		{
			seenImported = resources.GetTexture(exported);
		});

		graph.Compile();

		MockDevice device;
		MockCommandList commandList;
		graph.Execute(device, commandList, 0u);

		CHECK(seenImported.Index == 7);
	}

	TEST_CASE("rendergraph: a seed that reads what it writes is not double-counted")
	{
		rg::TransientTexturePool pool;
		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture other = graph.CreateTexture({});

		// Regression: the liveness propagation once re-added seed passes
		// whose own write matched their own read (they were not marked live
		// before the propagation ran), inflating the live count past the
		// topological order and tripping the cycle assert.
		auto& selfPass = graph.AddPass("SelfReadWrite");
		selfPass.Read(other, rhi::TextureState::ShaderRead);
		selfPass.Write(other, rhi::TextureState::RenderTarget);
		selfPass.Write(exported, rhi::TextureState::Present);
		selfPass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});

		graph.Compile();

		REQUIRE(graph.GetCompiledPasses().size() == 1);
		CHECK(NameAt(graph, 0) == "SelfReadWrite");
	}

	TEST_CASE("rendergraph: RecordPass fans out onto workers over pre-created transients")
	{
		// The concurrent record contract - the prepare stage pre-creates the
		// transients, then RecordPass fans out to loom workers with one mock
		// list per pass, and the final-state carry runs single-threaded
		// afterwards. Runs under TSAN in the tinytest dev-tsan config.
		loom::Shutdown();
		loom::Init();

		rg::TransientTexturePool pool;
		MockDevice device;

		std::atomic<int> executed{ 0 };

		rg::RenderGraph graph(pool);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		rg::RGTexture scene = graph.CreateTexture({});
		rg::RGTexture view = graph.CreateTexture({});

		auto& scenePass = graph.AddPass("Scene");
		scenePass.Write(scene, rhi::TextureState::RenderTarget);
		scenePass.SetExecute([&](rhi::CommandList&, const rg::PassResources&) { executed++; });

		auto& viewPass = graph.AddPass("View");
		viewPass.Read(scene, rhi::TextureState::ShaderRead);
		viewPass.Write(view, rhi::TextureState::RenderTarget);
		viewPass.SetExecute([&](rhi::CommandList&, const rg::PassResources&) { executed++; });

		auto& blend = graph.AddPass("Blend");
		blend.Read(view, rhi::TextureState::ShaderRead);
		blend.Write(exported, rhi::TextureState::Present);
		blend.SetExecute([&](rhi::CommandList&, const rg::PassResources&) { executed++; });

		graph.Compile();

		// The prepare stage creates every transient up front, single-threaded.
		graph.PreCreateTransients(device);
		CHECK(device.TextureCreates == 2);

		// One list per pass; the workers write disjoint slots.
		std::vector<MockCommandList> lists(graph.GetCompiledPassCount());
		loom::ParallelFor(0, (size_t)graph.GetCompiledPassCount(), 1,
			[&](size_t begin, size_t end)
			{
				for (size_t i = begin; i < end; i++)
					graph.RecordPass((uint32_t)i, device, lists[i], 0u);
			});

		// The single-threaded frame-end carry.
		graph.RecordFinalStates();

		CHECK(executed.load() == 3);

		// The fan-out emitted exactly the compiled barrier edges (each pass
		// re-issued its own) - the same multiset the sequential Execute
		// would have produced.
		std::vector<std::pair<rhi::TextureState, rhi::TextureState>> emitted;
		for (const MockCommandList& list : lists)
			emitted.insert(emitted.end(), list.Barriers.begin(), list.Barriers.end());
		CHECK(emitted.size() == graph.GetCompiledBarriers().size());
		for (const rg::RenderGraph::CompiledBarrier& barrier : graph.GetCompiledBarriers())
		{
			auto it = std::find(emitted.begin(), emitted.end(),
				std::make_pair(barrier.From, barrier.To));
			CHECK(it != emitted.end());
			if (it != emitted.end())
				emitted.erase(it);
		}
		CHECK(emitted.empty());

		// A second graph sharing the pool under its own graph id never
		// aliases the first graph's virtual indices.
		{
			rg::RenderGraph other(pool, /*graphId*/ 1);
			rg::RGTexture ownScene = other.CreateTexture({});
			auto& ownPass = other.AddPass("Own Scene");
			ownPass.Write(ownScene, rhi::TextureState::RenderTarget);
			ownPass.SetExecute([](rhi::CommandList&, const rg::PassResources&) {});
			other.Compile();
			other.PreCreateTransients(device);
			CHECK(device.TextureCreates == 3); // a fresh texture, not the graph-0 one
		}

		loom::Shutdown();
	}

	// M-A: the record-context contract - the graph scopes each pass with
	// the host-wired enter/leave hooks (the backend's thread-local frame
	// index) and carries the slot in PassResources::FrameSlot, so
	// record-side readers always resolve THIS job's slot. The tests
	// simulate the backend shim with their own thread-local - the graph
	// itself stays backend-neutral.
	namespace {
		thread_local uint32_t t_RecordContext = ~0u;
		uint32_t TestEnterRecordContext(uint32_t slot) { uint32_t prev = t_RecordContext; t_RecordContext = slot; return prev; }
		void TestLeaveRecordContext(uint32_t prev) { t_RecordContext = prev; }
	}

	TEST_CASE("rendergraph: RecordPass scopes the record context and PassResources carries the slot")
	{
		rg::TransientTexturePool pool;
		MockDevice device;
		MockCommandList list;

		rg::RenderGraph graph(pool);
		graph.SetRecordContextHooks(&TestEnterRecordContext, &TestLeaveRecordContext);
		rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);
		auto& pass = graph.AddPass("Scoped");
		pass.Write(exported, rhi::TextureState::Present);
		pass.SetExecute([](rhi::CommandList&, const rg::PassResources& resources) {
			CHECK(resources.FrameSlot == 7u);
			CHECK(t_RecordContext == 7u);
		});
		graph.Compile();

		t_RecordContext = ~0u;
		graph.RecordPass(0, device, list, 7u);
		CHECK(t_RecordContext == ~0u); // restored by the scope's exit
	}

	TEST_CASE("rendergraph: an interleaved record job restores the outer context (help-while-waiting)")
	{
		// Loom's help-while-waiting can run a SECOND record job on the same
		// worker while a pass waits on a counter; the per-pass scope must
		// restore the outer job's context when the nested job exits.
		loom::Shutdown();
		loom::Init();

		{ // the counters die inside this scope, before the pool shuts down
			rg::TransientTexturePool pool;
			MockDevice device;
			std::vector<MockCommandList> lists(2);

			rg::RenderGraph graph(pool);
			graph.SetRecordContextHooks(&TestEnterRecordContext, &TestLeaveRecordContext);
			rg::RGTexture exported = graph.Import(ImportHandle(1), rhi::TextureState::Undefined);

			std::atomic<uint32_t> ctxBefore{ 0 }, ctxNested{ 0 }, ctxAfter{ 0 };
			loom::ScopedCounter nestedDone(1);

			auto& outerPass = graph.AddPass("Outer"); // recorded at slot 3
			outerPass.Write(exported, rhi::TextureState::Present);
			outerPass.SetExecute([&](rhi::CommandList&, const rg::PassResources& resources) {
				ctxBefore = t_RecordContext;
				CHECK(resources.FrameSlot == 3u);
				// While this worker waits, it helps the pool: the nested job
				// (slot 5) runs here, scoped and restored by its own guard.
				// The nested task signals AFTER its RecordPass returns, so
				// the wait ends only once the nested scope has exited and
				// restored.
				loom::WaitFor(nestedDone.Get());
				ctxAfter = t_RecordContext;
			});

			auto& nestedPass = graph.AddPass("Nested"); // recorded at slot 5
			nestedPass.Write(exported, rhi::TextureState::Present);
			nestedPass.SetExecute([&](rhi::CommandList&, const rg::PassResources& resources) {
				ctxNested = t_RecordContext;
				CHECK(resources.FrameSlot == 5u);
			});

			graph.Compile();
			graph.PreCreateTransients(device);

			t_RecordContext = ~0u;
			loom::ScopedCounter outerDone(1);
			loom::Submit([&]() {
				graph.RecordPass(0, device, lists[0], 3u);
				loom::Signal(outerDone.Get());
			});
			loom::Submit([&]() {
				graph.RecordPass(1, device, lists[1], 5u);
				loom::Signal(nestedDone.Get());
			});
			loom::WaitFor(outerDone.Get());

			CHECK(ctxBefore.load() == 3u);
			CHECK(ctxNested.load() == 5u);
			CHECK(ctxAfter.load() == 3u); // the nested job's scope restored the outer context
			CHECK(t_RecordContext == ~0u);
		}

		loom::Shutdown();
	}

} // namespace rg_tests
} // namespace TinyEngine

