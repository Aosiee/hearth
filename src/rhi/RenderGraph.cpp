#include "tepch.h"
#include "TinyEngine/RenderGraph/RenderGraph.h"

#include <algorithm>

namespace TinyEngine {
namespace rg {

	using namespace rhi;

	// ---- TransientTexturePool ----------------------------------------------------

	TextureHandle TransientTexturePool::GetOrCreate(Device& device, uint64_t key, const TextureDesc& desc, const char* debugName)
	{
		auto [it, inserted] = m_Entries.emplace(key, Entry{});
		Entry& entry = it->second;
		if (inserted || !IsValid(entry.Handle))
		{
			entry.Desc = desc;
			entry.Handle = device.CreateTexture(desc, nullptr, 0, debugName);
		}
		return entry.Handle;
	}

	TextureHandle TransientTexturePool::GetHandle(uint64_t key) const
	{
		auto it = m_Entries.find(key);
		TE_CORE_ASSERT(it != m_Entries.end() && IsValid(it->second.Handle),
			"RecordPass resolved a transient that PreCreateTransients never created - the threaded host must pre-create transients before the fan-out!");
		return it->second.Handle;
	}

	TextureState TransientTexturePool::GetLastState(uint64_t key) const
	{
		auto it = m_Entries.find(key);
		if (it == m_Entries.end() || !IsValid(it->second.Handle))
			return TextureState::Undefined;
		return it->second.LastState;
	}

	void TransientTexturePool::SetLastState(uint64_t key, TextureState state)
	{
		auto it = m_Entries.find(key);
		if (it != m_Entries.end())
			it->second.LastState = state;
	}

	// ---- RenderGraph ------------------------------------------------------------

	RenderGraph::RenderGraph(TransientTexturePool& pool, uint32_t graphId)
		: m_Pool(pool)
		, m_GraphId(graphId)
	{
	}

	RGTexture RenderGraph::Import(TextureHandle handle, TextureState currentState)
	{
		VirtualTexture texture;
		texture.Handle = handle;
		texture.State = currentState;
		texture.Imported = true;
		m_Textures.push_back(texture);
		return { (uint32_t)m_Textures.size() - 1 };
	}

	RGTexture RenderGraph::CreateTexture(const TextureDesc& desc)
	{
		VirtualTexture texture;
		texture.Desc = desc;
		// The physical texture may already exist from a previous frame; its
		// carried state seeds the derivation at Compile time.
		texture.State = m_Pool.GetLastState(TransientTexturePool::MakeKey(m_GraphId, (uint32_t)m_Textures.size()));
		m_Textures.push_back(texture);
		return { (uint32_t)m_Textures.size() - 1 };
	}

	RenderGraph::PassBuilder::PassBuilder(RenderGraph& graph, uint32_t passIndex)
		: m_Graph(&graph), m_PassIndex(passIndex)
	{
	}

	RGTexture RenderGraph::PassBuilder::Read(RGTexture texture, TextureState state)
	{
		TE_CORE_ASSERT(texture.Valid(), "reading an invalid virtual texture");
		AccessRecord access;
		access.Texture = texture;
		access.State = state;
		access.Write = false;
		m_Graph->m_Passes[m_PassIndex].Accesses.push_back(access);
		return texture;
	}

	RGTexture RenderGraph::PassBuilder::Write(RGTexture texture, TextureState state)
	{
		TE_CORE_ASSERT(texture.Valid(), "writing an invalid virtual texture");
		AccessRecord access;
		access.Texture = texture;
		access.State = state;
		access.Write = true;
		m_Graph->m_Passes[m_PassIndex].Accesses.push_back(access);
		return texture;
	}

	void RenderGraph::PassBuilder::SetExecute(std::function<void(CommandList&, const PassResources&)> fn)
	{
		m_Graph->m_Passes[m_PassIndex].ExecuteFn = std::move(fn);
	}

	RenderGraph::PassBuilder& RenderGraph::AddPass(std::string_view name)
	{
		Pass pass;
		pass.Name = std::string(name);
		m_Passes.push_back(std::move(pass));
		// Direct `new` (not make_unique): the constructor is private to
		// RenderGraph and friendship does not extend to std::make_unique.
		m_Builders.push_back(std::unique_ptr<PassBuilder>(new PassBuilder(*this, (uint32_t)m_Passes.size() - 1)));
		return *m_Builders.back();
	}

	void RenderGraph::Compile()
	{
		m_Order.clear();
		m_CompiledPasses.clear();
		m_CompiledBarriers.clear();

		// ---- Culling: a pass is live if it writes an imported resource, or
		// a live pass reads something it wrote. Passes with a side effect
		// (an ExecuteFn but no accesses) stay live.
		for (Pass& pass : m_Passes)
			pass.Live = false;

		std::vector<uint32_t> live;
		for (uint32_t i = 0; i < m_Passes.size(); i++)
		{
			bool writesImport = false;
			for (const AccessRecord& access : m_Passes[i].Accesses)
			{
				if (access.Write && m_Textures[access.Texture.Index].Imported)
					writesImport = true;
			}
			if (writesImport || (m_Passes[i].Accesses.empty() && m_Passes[i].ExecuteFn))
			{
				m_Passes[i].Live = true; // mark immediately so the propagation below never re-adds a seed
				live.push_back(i);
			}
		}

		// Propagate liveness backward: any pass that wrote a texture a live
		// pass reads is itself live.
		for (size_t cursor = 0; cursor < live.size(); cursor++)
		{
			const Pass& consumer = m_Passes[live[cursor]];
			for (const AccessRecord& access : consumer.Accesses)
			{
				if (access.Write)
					continue;
				for (uint32_t i = 0; i < m_Passes.size(); i++)
				{
					if (m_Passes[i].Live)
						continue;
					for (const AccessRecord& produced : m_Passes[i].Accesses)
					{
						if (produced.Write && produced.Texture == access.Texture)
						{
							m_Passes[i].Live = true;
							live.push_back(i);
							break;
						}
					}
					if (m_Passes[i].Live)
						break;
				}
			}
		}

		// ---- Topological order (stable Kahn's): a pass depends on every
		// pass that wrote a texture it reads.
		std::vector<uint32_t> inDegree(m_Passes.size(), 0);
		std::vector<std::vector<uint32_t>> dependents(m_Passes.size());
		for (uint32_t i = 0; i < m_Passes.size(); i++)
		{
			if (!m_Passes[i].Live)
				continue;
			for (const AccessRecord& access : m_Passes[i].Accesses)
			{
				if (access.Write)
					continue;
				for (uint32_t j = 0; j < m_Passes.size(); j++)
				{
					if (!m_Passes[j].Live || j == i)
						continue;
					for (const AccessRecord& produced : m_Passes[j].Accesses)
					{
						if (produced.Write && produced.Texture == access.Texture)
						{
							dependents[j].push_back(i);
							inDegree[i]++;
						}
					}
				}
			}
		}

		std::vector<uint32_t> ready;
		for (uint32_t i = 0; i < m_Passes.size(); i++)
		{
			if (m_Passes[i].Live && inDegree[i] == 0)
				ready.push_back(i);
		}

		while (!ready.empty())
		{
			// Stable: pop the smallest index (registration order).
			auto smallest = std::min_element(ready.begin(), ready.end());
			uint32_t i = *smallest;
			ready.erase(smallest);
			m_Order.push_back(i);
			for (uint32_t dependent : dependents[i])
			{
				if (--inDegree[dependent] == 0)
					ready.push_back(dependent);
			}
		}

		TE_CORE_ASSERT(m_Order.size() == live.size(), "RenderGraph contains a dependency cycle - the graph must be acyclic");

		// ---- Barrier derivation: walk the ordered accesses per texture,
		// tracking state; a state change between consecutive users is a
		// barrier edge recorded against the later pass.
		std::vector<TextureState> state(m_Textures.size());
		for (uint32_t i = 0; i < m_Textures.size(); i++)
			state[i] = m_Textures[i].State;

		for (uint32_t orderIndex = 0; orderIndex < m_Order.size(); orderIndex++)
		{
			Pass& pass = m_Passes[m_Order[orderIndex]];
			for (const AccessRecord& access : pass.Accesses)
			{
				TextureState before = state[access.Texture.Index];
				if (before != access.State)
				{
					CompiledBarrier barrier;
					barrier.PassName = pass.Name;
					barrier.OrderIndex = orderIndex;
					barrier.TextureIndex = access.Texture.Index;
					barrier.From = before;
					barrier.To = access.State;
					m_CompiledBarriers.push_back(barrier);
				}
				state[access.Texture.Index] = access.State;
			}
		}

		for (uint32_t i : m_Order)
			m_CompiledPasses.push_back({ m_Passes[i].Name });

		// Keep the final per-texture states for the frame-end carry.
		m_FinalStates = state;
	}

	TextureState RenderGraph::GetFinalState(RGTexture texture) const
	{
		TE_CORE_ASSERT(texture.Valid(), "querying an invalid virtual texture");
		if (!m_FinalStates.empty() && texture.Index < m_FinalStates.size())
			return m_FinalStates[texture.Index];
		return m_Textures[texture.Index].State;
	}

	// T-F-B.3: the prepare-stage creation. Single-threaded by contract (the
	// host calls it after Compile, before the fan-out).
	void RenderGraph::PreCreateTransients(Device& device)
	{
		for (uint32_t i = 0; i < m_Textures.size(); i++)
		{
			if (!m_Textures[i].Imported)
				m_Pool.GetOrCreate(device, TransientTexturePool::MakeKey(m_GraphId, i), m_Textures[i].Desc, "RenderGraph Transient");
		}
	}

	// T-F-B.3: the single-threaded frame-end carry.
	void RenderGraph::RecordFinalStates()
	{
		for (uint32_t i = 0; i < m_Textures.size(); i++)
		{
			if (!m_Textures[i].Imported)
				m_Pool.SetLastState(TransientTexturePool::MakeKey(m_GraphId, i), m_FinalStates[i]);
		}
	}

	PassResources RenderGraph::ResolveResources(Device& device, const Pass& pass)
	{
		(void)device; // T-F-B.3: resolution is a pure pool lookup now; the
					  // parameter stays for the host-facing RecordPass surface.
		PassResources resources;
		for (const AccessRecord& access : pass.Accesses)
		{
			// One entry per distinct texture.
			bool found = false;
			for (PassResources::TextureRef& ref : resources.Textures)
			{
				if (ref.Virtual == access.Texture)
				{
					found = true;
					break;
				}
			}
			if (found)
				continue;

			PassResources::TextureRef ref;
			ref.Virtual = access.Texture;
			if (m_Textures[access.Texture.Index].Imported)
				ref.Handle = m_Textures[access.Texture.Index].Handle;
			else
				ref.Handle = m_Pool.GetHandle(TransientTexturePool::MakeKey(m_GraphId, access.Texture.Index));
			resources.Textures.push_back(ref);
		}
		return resources;
	}

	// The per-pass record: the pass's compiled barriers (the schedule
	// derived at Compile - one Barrier call) + its ExecuteFn. The barrier
	// set for the pass is its CompiledBarrier records (each one texture);
	// they batch into one Barrier call exactly like Execute did. The
	// transient handles resolve here via the pool's PURE LOOKUP - a HOST
	// that fans the passes onto workers must pre-create the transients via
	// PreCreateTransients (the prepare stage) before the fan-out.
	void RenderGraph::RecordPass(uint32_t orderIndex, Device& device, CommandList& commandList)
	{
		Pass& pass = m_Passes[m_Order[orderIndex]];

		// The barrier descriptors point into the textureBarriers vector -
		// the reserve prevents reallocation invalidating them mid-pass (the
		// same contract the old Execute held).
		std::vector<BarrierDesc> barriers;
		std::vector<TextureBarrier> textureBarriers;
		barriers.reserve(m_CompiledBarriers.size());
		textureBarriers.reserve(m_CompiledBarriers.size());
		for (const CompiledBarrier& compiled : m_CompiledBarriers)
		{
			if (compiled.OrderIndex != orderIndex)
				continue;

			textureBarriers.push_back({});
			TextureBarrier& textureBarrier = textureBarriers.back();
			textureBarrier.Texture = m_Textures[compiled.TextureIndex].Imported
				? m_Textures[compiled.TextureIndex].Handle
				: m_Pool.GetHandle(TransientTexturePool::MakeKey(m_GraphId, compiled.TextureIndex));
			textureBarrier.From = compiled.From;
			textureBarrier.To = compiled.To;
			textureBarrier.BaseMip = 0;
			textureBarrier.MipCount = 1;
			textureBarrier.BaseLayer = 0;
			textureBarrier.LayerCount = 1;

			BarrierDesc barrierDesc;
			barrierDesc.Textures = &textureBarrier;
			barrierDesc.TextureCount = 1;
			barriers.push_back(barrierDesc);
		}
		if (!barriers.empty())
			commandList.Barrier(barriers);

		if (pass.ExecuteFn)
		{
			PassResources resources = ResolveResources(device, pass);
			pass.ExecuteFn(commandList, resources);
		}
	}

	void RenderGraph::Execute(Device& device, CommandList& commandList)
	{
		// The per-texture state machine, walked identically to Compile so
		// the emitted edges match GetCompiledBarriers() exactly - including
		// passes without a lambda (their declarations still move state).
		// T-F-B: the sequential Execute drives the same per-pass RecordPass
		// surface the threaded host fans out - one code path for the
		// barrier derivation. It pre-creates its transients up front (the
		// sequential path is single-threaded either way) and carries the
		// final states at its tail, exactly like the threaded host.
		PreCreateTransients(device);

		for (uint32_t orderIndex = 0; orderIndex < m_Order.size(); orderIndex++)
			RecordPass(orderIndex, device, commandList);

		// Record the transients' final states so the next frame's graph
		// seeds its derivation from where this frame left them.
		RecordFinalStates();
	}

} // namespace rg
} // namespace TinyEngine
