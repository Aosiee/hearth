#pragma once

// The RenderGraph core. Backend-neutral and headless-safe: passes declare
// reads/writes of virtual resources (textures and buffers) with their access
// state; a pass's ExecuteFn records whatever it needs (a draw inside its own
// BeginRendering, a compute dispatch, a copy). Compile
// produces a topological order, culls passes whose outputs are never
// consumed, derives the minimal barrier set between consecutive users, and
// Execute replays the plan on one CommandList (single-threaded; RecordPass
// is the per-pass surface a host fans out). No aliasing - every virtual
// transient owns its own physical texture,
// reused across frames through the pool (which also carries each texture's
// state forward so the next frame's derivation seeds from where the last
// one left it).

#include "hearth/rhi/RHI.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <memory>

namespace hearth::rg {

	// The graph's vocabulary IS the RHI's (handles, states, barriers).
	using namespace rhi;

	struct RGTexture
	{
		uint32_t Index = ~0u;
		bool Valid() const { return Index != ~0u; }
		bool operator==(const RGTexture& other) const { return Index == other.Index; }
	};

	// An imported buffer (a compute pass's output, a vertex stream a draw
	// reads). Buffers are imported only: the graph orders and fences them, it
	// does not allocate them.
	struct RGBuffer
	{
		uint32_t Index = ~0u;
		bool Valid() const { return Index != ~0u; }
		bool operator==(const RGBuffer& other) const { return Index == other.Index; }
	};

	// The concrete resources one pass reads/writes, resolved at Execute.
	struct PassResources
	{
		struct BufferRef
		{
			RGBuffer Virtual;
			BufferHandle Handle;
		};
		std::vector<BufferRef> Buffers;

		BufferHandle GetBuffer(RGBuffer virtualBuffer) const
		{
			for (const BufferRef& ref : Buffers)
			{
				if (ref.Virtual == virtualBuffer)
					return ref.Handle;
			}
			return {};
		}

		struct TextureRef
		{
			RGTexture Virtual;
			TextureHandle Handle;
		};
		std::vector<TextureRef> Textures;

		// No frame slot here - the slot is the command list's
		// (CommandList::FrameSlot, the slot of the pool the host allocated
		// the list from). One source, so the two can never disagree; the
		// graph itself is slot-agnostic.

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
	// aliasing. The key namespaces on the graph id so two graphs sharing one
	// pool can never alias a virtual index.
	class TransientTexturePool
	{
	public:
		static uint64_t MakeKey(uint32_t graphId, uint32_t textureIndex)
		{
			return ((uint64_t)graphId << 32) | textureIndex;
		}

		// The prepare-stage creation: called on the record host's
		// thread before the fan-out - never from a RecordPass.
		TextureHandle GetOrCreate(Device& device, uint64_t key, const TextureDesc& desc, const char* debugName);
		// The record-stage lookup: pure read, asserts the prepare stage
		// created the entry (RecordPass must never create).
		TextureHandle GetHandle(uint64_t key) const;
		TextureState GetLastState(uint64_t key) const;
		void SetLastState(uint64_t key, TextureState state);

	private:
		struct Entry
		{
			TextureDesc Desc;
			TextureHandle Handle;
			TextureState LastState = TextureState::Undefined;
		};
		std::unordered_map<uint64_t, Entry> m_Entries;
	};

	class RenderGraph
	{
	public:
		explicit RenderGraph(TransientTexturePool& pool, uint32_t graphId = 0);
		~RenderGraph() = default;
		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;

		// External resources (the swapchain image, framebuffer targets,
		// the program's own textures): the graph tracks their state from `currentState`
		// and derives barriers against it. The final state at the end of the
		// frame is whatever the last user leaves.
		RGTexture Import(TextureHandle handle, TextureState currentState);

		// A transient resource, pooled across frames by its virtual index.
		RGTexture CreateTexture(const TextureDesc& desc);

		// An external buffer, tracked from `currentState` like an imported
		// texture (a write to it keeps its pass live).
		RGBuffer ImportBuffer(BufferHandle handle, BufferState currentState);

		class PassBuilder
		{
		public:
			// Declares the state the texture is in FOR this access (a read
			// leaves it there; a write ends with it there). The graph derives
			// from -> to barriers against the previous user's state.
			RGTexture Read(RGTexture texture, TextureState state);
			RGTexture Write(RGTexture texture, TextureState state);
			RGBuffer Read(RGBuffer buffer, BufferState state);
			RGBuffer Write(RGBuffer buffer, BufferState state);
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

		// The prepare stage creates every transient texture up
		// front (single-threaded, on the record host) so RecordPass is a
		// pure read and can fan out to workers. Call after Compile.
		void PreCreateTransients(Device& device);

		// The single-threaded frame-end carry - the transients'
		// final states feed the next frame's seeding. Execute calls it at
		// its tail; the threaded host calls it after the fan-out.
		void RecordFinalStates();

		// Replays the compiled plan: barriers + each pass's lambda with its
		// resolved resources. Compile must have run. Records the transients'
		// final states back into the pool. Every pass records into
		// `commandList` (and reads its frame slot from it).
		void Execute(Device& device, CommandList& commandList);

		// The per-pass record surface - the host fans RecordPass out to
		// workers and submits the lists in the compiled order. Each pass
		// re-issues its compiled barriers (the schedule derived at Compile)
		// + its ExecuteFn into the given command list. RecordPass itself is
		// single-threaded per call; the concurrency contract is on the host
		// (and on the transient pool, which the prepare stage pre-creates -
		// see RecordPass's comment). The pass's frame slot is
		// `commandList.FrameSlot()` - the record context travels with the
		// list the ExecuteFn receives (no ambient state), so a nested job on
		// the same worker (Loom's help-while-waiting) or a child job the
		// ExecuteFn fans out to reads the slot it was handed.
		void RecordPass(uint32_t orderIndex, Device& device, CommandList& commandList);

		uint32_t GetCompiledPassCount() const { return (uint32_t)m_Order.size(); }
		uint32_t CompiledPassIndex(uint32_t orderIndex) const { return m_Order[orderIndex]; }

		// The compiled plan (the tests and GpuFrameDebugInfo read these).
		struct CompiledPass
		{
			std::string Name;
		};
		struct CompiledBarrier
		{
			std::string PassName;
			// The pass's position in the compiled order (NOT its name - a
			// program may register same-named passes per framebuffer
			// target, so the name is not unique; RecordPass keys on this).
			uint32_t OrderIndex = 0;
			// A texture barrier (TextureIndex, From, To) or, with IsBuffer, a
			// buffer one (BufferIndex, BufferFrom, BufferTo).
			bool IsBuffer = false;
			uint32_t TextureIndex = 0;
			TextureState From = TextureState::Undefined;
			TextureState To = TextureState::Undefined;
			uint32_t BufferIndex = 0;
			BufferState BufferFrom = BufferState::Undefined;
			BufferState BufferTo = BufferState::Undefined;

			const char* FromName() const { return IsBuffer ? BufferStateName(BufferFrom) : TextureStateName(From); }
			const char* ToName() const { return IsBuffer ? BufferStateName(BufferTo) : TextureStateName(To); }
		};
		const std::vector<CompiledPass>& GetCompiledPasses() const { return m_CompiledPasses; }
		const std::vector<CompiledBarrier>& GetCompiledBarriers() const { return m_CompiledBarriers; }
		// The compiled ordering edges, by registration index: true when
		// pass `later` must run after pass `earlier` (tests + diagnostics).
		bool DependsOn(uint32_t later, uint32_t earlier) const
		{
			return earlier < m_Dependents.size()
				&& std::find(m_Dependents[earlier].begin(), m_Dependents[earlier].end(), later) != m_Dependents[earlier].end();
		}

		// The state each texture is in after the last ordered access (its
		// initial state if never accessed) - the frame-end carry writes
		// these back to the external trackers so the next frame's Imports
		// seed correctly.
		TextureState GetFinalState(RGTexture texture) const;
		BufferState GetFinalState(RGBuffer buffer) const;

	private:
		// One access to a texture or (IsBuffer) a buffer - index into
		// m_Textures or m_Buffers. Culling, ordering and the barrier walk
		// treat both kinds alike.
		struct AccessRecord
		{
			bool IsBuffer = false;
			uint32_t Index = ~0u;
			TextureState State = TextureState::Undefined;
			BufferState BufState = BufferState::Undefined;
			bool Write = false;

			bool SameResource(const AccessRecord& other) const { return IsBuffer == other.IsBuffer && Index == other.Index; }
		};

		struct VirtualBuffer
		{
			BufferHandle Handle;
			BufferState State = BufferState::Undefined;
		};

		bool IsImported(const AccessRecord& access) const { return access.IsBuffer || m_Textures[access.Index].Imported; }

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
		uint32_t m_GraphId = 0;
		std::vector<Pass> m_Passes;
		std::vector<VirtualTexture> m_Textures;
		std::vector<VirtualBuffer> m_Buffers;
		std::vector<std::unique_ptr<PassBuilder>> m_Builders;
		std::vector<uint32_t> m_Order;
		std::vector<CompiledPass> m_CompiledPasses;
		std::vector<CompiledBarrier> m_CompiledBarriers;
		std::vector<std::vector<uint32_t>> m_Dependents;
		// The per-pass barrier ranges into m_CompiledBarriers (which is
		// appended in ascending OrderIndex order): m_PassBarrierBegin[i] is
		// the first barrier of compiled pass i, [i+1] the exclusive end.
		std::vector<uint32_t> m_PassBarrierBegin;
		std::vector<TextureState> m_FinalStates;
		std::vector<BufferState> m_FinalBufferStates;
	};

} // namespace hearth::rg
