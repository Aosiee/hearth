#pragma once

// The RenderGraph core. Backend-neutral and headless-safe: passes declare
// reads/writes of virtual resources with their access state; Compile
// produces a topological order, culls passes whose outputs are never
// consumed, derives the minimal barrier set between consecutive users, and
// Execute replays the plan on a CommandList. v1: single-threaded Execute,
// no aliasing - every virtual transient owns its own physical texture,
// reused across frames through the pool (which also carries each texture's
// state forward so the next frame's derivation seeds from where the last
// one left it).

#include "TinyEngine/RHI/RHI.h"

#include <functional>
#include <string>
#include <vector>

namespace TinyEngine {
namespace rg {

	// The graph's vocabulary IS the RHI's (handles, states, barriers).
	using namespace rhi;

	struct RGTexture
	{
		uint32_t Index = ~0u;
		bool Valid() const { return Index != ~0u; }
		bool operator==(const RGTexture& other) const { return Index == other.Index; }
	};

	// The concrete resources one pass reads/writes, resolved at Execute.
	struct PassResources
	{
		struct TextureRef
		{
			RGTexture Virtual;
			TextureHandle Handle;
		};
		std::vector<TextureRef> Textures;

		TextureHandle GetTexture(RGTexture virtualTexture) const
		{
			for (const TextureRef& ref : Textures)
			{
				if (ref.Virtual == virtualTexture)
					return ref.Handle;
			}
			return {};
		}
	};

	// Owns the physical transient textures across frames. One texture per
	// stable key (the graph's virtual index - built identically each frame),
	// allocated lazily; each entry carries the state its last user left it
	// in, so the next frame's barrier derivation seeds correctly. No
	// aliasing in v1.
	class TransientTexturePool
	{
	public:
		TextureHandle GetOrCreate(Device& device, uint32_t key, const TextureDesc& desc, const char* debugName);
		TextureState GetLastState(uint32_t key) const;
		void SetLastState(uint32_t key, TextureState state);

	private:
		struct Entry
		{
			TextureDesc Desc;
			TextureHandle Handle;
			TextureState LastState = TextureState::Undefined;
		};
		std::vector<Entry> m_Entries; // indexed by key (sparse, grows on demand)
	};

	class RenderGraph
	{
	public:
		explicit RenderGraph(TransientTexturePool& pool);
		~RenderGraph() = default;
		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;

		// External resources (the swapchain image, framebuffer targets,
		// engine textures): the graph tracks their state from `currentState`
		// and derives barriers against it. The final state at the end of the
		// frame is whatever the last user leaves.
		RGTexture Import(TextureHandle handle, TextureState currentState);

		// A transient resource, pooled across frames by its virtual index.
		RGTexture CreateTexture(const TextureDesc& desc);

		class PassBuilder
		{
		public:
			// Declares the state the texture is in FOR this access (a read
			// leaves it there; a write ends with it there). The graph derives
			// from -> to barriers against the previous user's state.
			RGTexture Read(RGTexture texture, TextureState state);
			RGTexture Write(RGTexture texture, TextureState state);
			void SetExecute(std::function<void(CommandList&, const PassResources&)> fn);

		private:
			friend class RenderGraph;
			PassBuilder(RenderGraph& graph, uint32_t passIndex);
			RenderGraph* m_Graph = nullptr;
			uint32_t m_PassIndex = 0;
		};

		PassBuilder& AddPass(std::string_view name);

		// Orders the live passes, culls dead ones, and derives the barriers.
		// The transient states seed from the pool's recorded last states.
		void Compile();

		// Replays the compiled plan: barriers + each pass's lambda with its
		// resolved resources. Compile must have run. Records the transients'
		// final states back into the pool.
		void Execute(Device& device, CommandList& commandList);

		// The compiled plan (the tests and GpuFrameDebugInfo read these).
		struct CompiledPass
		{
			std::string Name;
		};
		struct CompiledBarrier
		{
			std::string PassName;
			uint32_t TextureIndex = 0;
			TextureState From = TextureState::Undefined;
			TextureState To = TextureState::Undefined;
		};
		const std::vector<CompiledPass>& GetCompiledPasses() const { return m_CompiledPasses; }
		const std::vector<CompiledBarrier>& GetCompiledBarriers() const { return m_CompiledBarriers; }

		// The state each texture is in after the last ordered access (its
		// initial state if never accessed) - the frame-end carry writes
		// these back to the external trackers so the next frame's Imports
		// seed correctly.
		TextureState GetFinalState(RGTexture texture) const;

	private:
		struct AccessRecord
		{
			RGTexture Texture;
			TextureState State = TextureState::Undefined;
			bool Write = false;
		};

		struct Pass
		{
			std::string Name;
			std::vector<AccessRecord> Accesses;
			std::function<void(CommandList&, const PassResources&)> ExecuteFn;
			bool Live = false;
		};

		struct VirtualTexture
		{
			TextureHandle Handle;      // the real handle (imports) or {} (transient, pooled)
			TextureDesc Desc;          // transient only
			TextureState State = TextureState::Undefined;
			bool Imported = false;
		};

		PassResources ResolveResources(Device& device, const Pass& pass);

		TransientTexturePool& m_Pool;
		std::vector<Pass> m_Passes;
		std::vector<VirtualTexture> m_Textures;
		std::vector<std::unique_ptr<PassBuilder>> m_Builders;
		std::vector<uint32_t> m_Order;
		std::vector<CompiledPass> m_CompiledPasses;
		std::vector<CompiledBarrier> m_CompiledBarriers;
		std::vector<TextureState> m_FinalStates;
	};

} // namespace rg
} // namespace TinyEngine
