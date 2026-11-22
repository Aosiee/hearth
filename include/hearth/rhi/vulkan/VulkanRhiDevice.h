#pragma once

// The Vulkan implementation of the RHI. Wrap-first: every handle wraps the
// existing Platform/Vulkan classes with no behaviour change. P-A1 ships
// the device lifecycle, the generation- checked handle tables, and the
// resource create/destroy surface; the frame-sync (P-A2) and bindless
// (P-A3) rewrites land on top.

#include "TinyEngine/RHI/RHI.h"

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/Device/VulkanDevice.h"
#include "Platform/Vulkan/Resources/VulkanBuffer.h"
#include "Platform/Vulkan/Resources/VulkanTexture.h"
#include "Platform/Vulkan/Shaders/VulkanPipeline.h"
#include "Platform/Vulkan/Shaders/VulkanShaderModule.h"
#include "Platform/Vulkan/Device/VulkanSwapchain.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <atomic>

struct GLFWwindow;

namespace TinyEngine {
namespace rhi {

	// The standalone sampler (the existing VulkanTexture2D bakes its own
	// internally; this is the RHI's general form, consumed by the fixed
	// sampler table in P-A3).
	class VulkanSampler
	{
	public:
		void Init(VulkanDevice& device, const SamplerDesc& desc, const std::string& debugName);
		void Shutdown();
		VkSampler GetHandle() const;

	private:
		VulkanDevice* m_Device = nullptr;
		VkSampler m_Sampler = VK_NULL_HANDLE;
	};

	// The command-list implementation: wraps one VkCommandBuffer and
	// resolves every handle through the owning device. Constructed with
	// the frame slot its buffer records for (CommandList::FrameSlot) -
	// the record job's (frame slot, record thread) pool slot. A seam
	// callback receives the pass's list itself (RenderPassContext); a
	// renderer that wraps a buffer it was handed builds its wrapper
	// from the list it got (same buffer, same slot), never from a slot
	// looked up elsewhere.
	class VulkanRhiDevice;
	class VulkanCommandList final : public CommandList
	{
	public:
		VulkanCommandList(VulkanRhiDevice* device, VkCommandBuffer commandBuffer, uint32_t frameSlot);

		void Begin() override;
		void End() override;
		void BeginRendering(std::span<const AttachmentDesc> attachments) override;
		void EndRendering() override;
		void Barrier(std::span<const BarrierDesc> barriers) override;
		void BindPipeline(PipelineHandle pipeline) override;
		void BindGlobalSet() override;
		void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) override;
		void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
		void SetDepthBias(float constantFactor, float clamp, float slopeFactor) override;
		void PushConstants(uint32_t stages, uint32_t offset, const void* data, uint32_t size) override;
		void BindVertexBuffer(BufferHandle buffer, uint64_t offset = 0) override;
		void BindIndexBuffer(BufferHandle buffer, uint64_t offset = 0) override;
		void Draw(uint32_t vertexCount, uint32_t instanceCount = 1,
			uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;
		void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
			uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;
		void Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ) override;
		void CopyBufferToBuffer(BufferHandle src, BufferHandle dst,
			uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override;
		void CopyBufferToTexture(BufferHandle src, TextureHandle dst,
			uint32_t width, uint32_t height) override;
		void CopyTextureToBuffer(TextureHandle src, BufferHandle dst,
			uint32_t width, uint32_t height) override;
		void BeginLabel(const char* name, bool timed = true) override;
		void EndLabel() override;
		void WriteTimestamp(uint32_t queryIndex) override;

		VkCommandBuffer GetCommandBuffer() const;

	private:
		VulkanRhiDevice* m_Device = nullptr;
		VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
		VkPipelineLayout m_BoundLayout = VK_NULL_HANDLE;
		VkPipelineBindPoint m_BoundBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	};

	// P-D: a wrapped external image (WrapUIImageHandle) - the caller owns the
	// VkImage; the RHI owns only the view + the bindless slot. Destruction
	// retires the view + slot (no image, no sampler - the shared fixed
	// sampler must never enter a retire entry). The image itself is recorded
	// so barrier targets resolve through the same handle (P4-B: the
	// swapchain images are adopted this way).
	class VulkanWrappedTexture
	{
	public:
		VulkanWrappedTexture(uint32_t bindlessIndex, VkImageView view, VkImage image)
			: BindlessIndex(bindlessIndex), View(view), Image(image) {}

		uint32_t GetBindlessIndex() const { return BindlessIndex; }
		VkImageView GetView() const { return View; }
		VkImage GetImage() const { return Image; }

	private:
		uint32_t BindlessIndex = 0;
		VkImageView View = VK_NULL_HANDLE;
		VkImage Image = VK_NULL_HANDLE;
	};

	// One TextureHandle space: an entry is EITHER an owned VulkanTexture2D
	// or a wrapped external image (never both - two tables sharing the same
	// handle type would alias {index, generation} and mis-resolve).
	struct VulkanTextureEntry
	{
		std::unique_ptr<VulkanTexture2D> Owned;
		std::unique_ptr<VulkanWrappedTexture> Wrapped;

		bool IsWrapped() const { return Wrapped != nullptr; }
		uint32_t GetBindlessIndex() const
		{
			return Wrapped ? Wrapped->GetBindlessIndex()
				: (Owned ? Owned->GetBindlessIndex() : 0);
		}
	};

	class VulkanRhiDevice final : public Device
	{
	public:
		VulkanRhiDevice();
		~VulkanRhiDevice() override;

		// The device lifecycle: lazy on first window (the window is needed
		// for GLFW's instance-extension query), destroyed by the subsystem
		// on shutdown.
		void EnsureDevice(GLFWwindow* windowForExtensionQuery);
		void ShutdownDevice();
		VulkanDevice* GetVulkan() { return &m_Vulkan; }

		// ---- P-A3: the global bindless set + the retire queue ----------
		// P-F: ONE UPDATE_AFTER_BIND set PER FRAME SLOT (TotalFrameSlots
		// identical copies - a slot's set is only touched when that slot's
		// previous submission has retired, so rewrites are legal). Every
		// descriptor write goes through a monotonic write log and is
		// applied to a slot's set as a single batch when that slot begins
		// recording; a texture's BindlessIndex is written once at creation
		// and recycled through the retire queue only after its tombstone
		// write has been applied to EVERY slot's set.
		static constexpr uint32_t kBindlessTextures = 16384;
		static constexpr uint32_t kBindlessBuffers = 4096;

		VkDescriptorSet GetGlobalDescriptorSet(uint32_t slot) const { return m_GlobalSets[slot]; }
		VkDescriptorSetLayout GetGlobalSetLayout() const { return m_GlobalSetLayout; }

		uint32_t AllocateBindlessTextureIndex();
		void WriteTextureDescriptor(uint32_t index, VkImageView imageView, VkSampler sampler, VkImageLayout layout);
		// The buffer side of the bindless set (binding 1, STORAGE_BUFFER) -
		// the per-frame light/view SSBO and the per-material SSBO are its
		// first consumers. The same contract as textures: write once
		// through the log, recycle the index only after every slot applied
		// its tombstone.
		uint32_t AllocateBindlessBufferIndex();
		void WriteBufferDescriptor(uint32_t index, VkBuffer buffer);
		// P-F: applies every logged write with Seq > this slot's last
		// applied seq to the slot's set in one vkUpdateDescriptorSets batch
		// (idempotent; called from VulkanCommandList::BindGlobalSet).
		void ApplyDescriptorLog(uint32_t slot);
		// A window registers its slots' base when it allocates them (the
		// GC floor's expected set); a closing window's slots deactivate
		// so their stale seq can't pin the floor.
		void NoteSlotExpected(uint32_t base);
		void DeactivateWindowSlots(uint32_t base);
		void QueueTextureForDestruction(VkImage image, VmaAllocation allocation,
			VkImageView imageView, VkSampler sampler, uint32_t bindlessIndex);
		// The submit thread's record-start reset notification - the slot's
		// command buffers (the last reference to the frame's resources)
		// are freed here, so the retire entries tagged for this reset can
		// free too.
		void NoteSlotReset(uint32_t slot);
		// A retire entry frees at the slot's Nth reset after it was queued
		// (QueueRetire tags it count + this). A releasing slot's owner calls
		// NoteSlotReset this many times once its GPU work is done, so every
		// entry still tagged to the slot frees.
		static constexpr uint64_t kRetireResetDistance = 2;
		void QueuePipelineForDestruction(VkPipeline pipeline, VkPipelineLayout layout);
		void QueueShaderForDestruction(VkShaderModule shader);
		void QueueSamplerForDestruction(VkSampler sampler);

		// Frees every entry whose slot's record-start resets reached its tag
		// (called once per frame by SwapBuffers, after the enqueue, and by the
		// 2D retire self-test - both on the game thread). Shutdown drains
		// unconditionally instead. The retire queue is game-thread-owned -
		// QueueRetire (the choke point every Queue*ForDestruction / Destroy* /
		// GPU-object destructor reaches) and both drains assert
		// VulkanDevice::IsGameThread(). Only NoteSlotReset crosses threads,
		// through the atomic reset counts.
		void DrainRetireQueue();
		void DrainRetireQueueUnconditional();

		// Dev/test accessor: true while the index is handed out and not yet
		// recycled by a drain (the self-test's assertions). An index
		// awaiting its tombstone's application is NOT live.
		bool IsBindlessIndexLive(uint32_t index) const;

		// Dev/test accessor: true once the index is back in the free list
		// - i.e. the timeline passed AND every slot applied its tombstone
		// (what the self-test asserts).
		bool IsBindlessIndexRecycled(uint32_t index) const;

		// Dev/test accessor: the retire queue's current depth (the S-C
		// buffer-retire self-test asserts the mesh destruction path queues
		// instead of destroying synchronously).
		size_t GetRetireQueueSize() const;

		// Dev/test accessors (the self-test's Retiring-slot checks):
		// owner-live = the owner-side lookup resolves (false from
		// DestroyBuffer on - a second destroy is rejected); recycled = the
		// table slot was released (generation bumped) and its index is back
		// in the free list.
		bool IsBufferOwnerLive(BufferHandle handle) const { return m_Buffers.Get(handle) != nullptr; }
		bool IsBufferSlotRecycled(BufferHandle handle) const;
		size_t GetPipelineCacheSize() const { return m_PipelineCache.size(); }
		// The reload no-op guard compares the re-read SPIR-V against
		// the shader's current module (nullptr when absent).
		const std::vector<uint32_t>* GetShaderSpirV(ShaderHandle handle) const
		{
			const VulkanShaderModule* shader = m_Shaders.Get(handle);
			return shader ? &shader->GetSpirV() : nullptr;
		}

		// ---- Device -------------------------------------------------------
		// creates exactly desc.Usage (a device-local buffer adds
		// TRANSFER_DST, its staging-upload target).
		BufferHandle CreateBuffer(const BufferDesc& desc, const char* debugName) override;
		// Retires the buffer and its table slot together through the retire
		// queue (game thread, like every QueueRetire). From this call on the
		// handle is dead to its owner (every owner-side lookup fails, a
		// second destroy asserts), but the record-side resolution
		// (GetBufferHandle) keeps resolving it until the free - the frame
		// still recording on the submit thread may name it (a PendingDraw
		// carries handles, not Refs).
		void DestroyBuffer(BufferHandle handle) override;
		void SetBufferData(BufferHandle handle, const void* data, uint64_t size, uint64_t offset = 0) override;
		// The capture sidecar's read of a host-visible buffer (null otherwise).
		const void* GetBufferMappedData(BufferHandle handle);
		// The bindless storage-buffer write (binding 1).
		void WriteBindlessBuffer(uint32_t index, BufferHandle handle) override;
		void UploadBufferViaStaging(BufferHandle handle, const void* data, uint64_t size) override;

		TextureHandle CreateTexture(const TextureDesc& desc, const void* initialData, uint64_t dataSize, const char* debugName) override;
		TextureHandle CreateTextureFromBakedMips(uint32_t width, uint32_t height,
			const std::vector<std::vector<uint8_t>>& mipLevels, const char* debugName) override;
		TextureHandle CreateTextureFromNativeImage(void* nativeImage, uint32_t width, uint32_t height, Format format) override;
		TextureHandle AdoptExternalImage(void* nativeImage) override;
		void DestroyTexture(TextureHandle handle) override;
		void SetTextureData(TextureHandle handle, const void* data, uint64_t size) override;
		void UpdateTextureBakedMips(TextureHandle handle, const std::vector<std::vector<uint8_t>>& mipLevels) override;
		uint32_t GetBindlessIndex(TextureHandle handle) override;

		SamplerHandle CreateSampler(const SamplerDesc& desc, const char* debugName) override;
		void DestroySampler(SamplerHandle handle) override;

		ShaderHandle CreateShader(std::span<const uint32_t> spirv, ShaderStage stage, const char* debugName, const ShaderIdentity* identity = nullptr) override;
		void DestroyShader(ShaderHandle handle) override;
		ShaderReloadResult ReloadShader(std::string_view name, std::span<const uint32_t> spirv) override;

		PipelineHandle CreatePipeline(const PipelineStateDesc& desc, const char* debugName) override;
		PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc, const char* debugName) override;
		void DestroyPipeline(PipelineHandle handle) override;
		void InvalidatePipelines(const AssetID& shaderAsset) override;

		CommandListHandle CreateCommandList(QueueHandle queue, uint32_t threadIndex) override;
		void DestroyCommandList(CommandListHandle handle) override;
		CommandList* GetCommandList(CommandListHandle handle) override;

		QueueHandle GetQueue(QueueKind kind) override;

		// The upload batch delegates to the VulkanDevice (see its own
		// comment - thread-local state, one submit without waitIdle).
		void BeginUploadBatch() override { m_Vulkan.BeginUploadBatch(); }
		void EndUploadBatch() override { m_Vulkan.EndUploadBatch(); }

		uint64_t Submit(QueueHandle queue, std::span<const CommandListHandle> commandLists,
			std::span<const TimelineWait> waits, std::span<const TimelineSignal> signals) override;

		void WaitIdle() override;

		SwapchainHandle CreateSwapchain(void* window) override;
		void DestroySwapchain(SwapchainHandle handle) override;

		// ---- Internal accessors the CommandList uses to resolve handles ----
		void SetClearColour(const float rgba[4])
		{
			for (int i = 0; i < 4; i++) m_ClearColour[i] = rgba[i];
		}
		const float* GetClearColour() const { return m_ClearColour; }
		VkBuffer GetBufferHandle(BufferHandle handle) const;
		VkImage GetTextureImage(TextureHandle handle) const;
		VkImageView GetTextureView(TextureHandle handle) const;
		VkExtent2D GetTextureSize(TextureHandle handle) const;
		VkPipeline GetPipelineHandle(PipelineHandle handle) const;
		VkPipelineLayout GetPipelineLayout(PipelineHandle handle) const;
		VkPipelineBindPoint GetPipelineBindPoint(PipelineHandle handle) const;

	private:
		// `slot` (DestroyBuffer's) is the RHI handle whose table slot
		// releases WITH the buffer at the free, so a handle a pipelined
		// record still names keeps resolving until then. Invalid = a raw
		// buffer with no live slot (the shutdown sweep). Private:
		// DestroyBuffer is the one entry point that marks and retires a table
		// slot.
		void QueueBufferForDestruction(VkBuffer buffer, VmaAllocation allocation, uint32_t bindlessIndex = ~0u, BufferHandle slot = {});

		// P-A3: the global bindless set and the retire
		// queue. The stamp is the value the NEXT submit will signal
		// - the earliest submission that could reference the object.
		void CreateGlobalBindlessSet();
		void DestroyGlobalBindlessSet();
		void CreateFixedSamplers();
		// P-F: the 1x1 white null texture every tombstone write points a
		// freed slot at (a REAL descriptor - nulling the slot instead
		// requires the nullDescriptor feature, and even enabled it faults
		// the current NVIDIA driver).
		void CreateNullTexture();
		void DestroyNullTexture();
		// The tombstone target for a freed buffer slot (a real 16-byte
		// buffer - nulling the descriptor needs the nullDescriptor
		// feature, which is not enabled).
		void CreateNullBuffer();
		void DestroyNullBuffer();

		// P-F: the descriptor write log. All writes - creation, rewrite,
		// tombstone - append here; nothing writes a set directly. A slot's
		// set applies its pending entries at record start
		// (ApplyDescriptorLog).
		struct DescriptorWriteLogEntry
		{
			enum class Kind : uint8_t { Texture, Buffer, Tombstone };

			Kind           Kind_ = Kind::Texture;
			uint64_t       Seq = 0;
			uint32_t       Index = 0; // array element in the bindless binding
			uint32_t       Binding = 0; // the bindless binding (0 = textures, 1 = buffers)
			VkImageView    ImageView = VK_NULL_HANDLE;
			VkSampler      Sampler = VK_NULL_HANDLE;
			VkImageLayout  ImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkBuffer       Buffer = VK_NULL_HANDLE;
		};

		// Mirrors a log append into the per-index current-descriptor table
		// (m_DescriptorLogMutex held).
		void RecordCurrentDescriptor(const DescriptorWriteLogEntry& entry);

		// The oldest applied seq across all slots - the log-entry GC and
		// the tombstone-recycle floor (callers hold m_DescriptorLogMutex).
		uint64_t MinLastAppliedSeq() const;
		// Moves every pending-reuse index whose tombstone every slot has
		// applied back to the free list (m_DescriptorLogMutex held).
		void TryRecyclePendingIndices();

		struct RetireEntry
		{
			enum class Kind : uint8_t { Texture, Buffer, Pipeline, Shader, Sampler };

			Kind        Kind_ = Kind::Texture;
			// The frame-boundary tag - the entry frees once the SLOT's
			// record-start resets (the NoteSlotReset bumps) reach the
			// ResetTag. No timeline prediction (the old last-submitted+1
			// stamp freed one submission early when an upload batch or
			// another window's submit interleaved).
			uint32_t    Slot = 0;
			uint64_t    ResetTag = 0;
			VkImage     Image = VK_NULL_HANDLE;
			VmaAllocation Allocation = VK_NULL_HANDLE;
			VkImageView ImageView = VK_NULL_HANDLE;
			VkSampler   Sampler = VK_NULL_HANDLE;
			VkBuffer    Buffer = VK_NULL_HANDLE;
			VkPipeline  Pipeline = VK_NULL_HANDLE;
			VkPipelineLayout Layout = VK_NULL_HANDLE;
			VkShaderModule ShaderModule = VK_NULL_HANDLE;
			// Buffer: the RHI handle whose (retiring) table slot releases
			// before the VkBuffer is destroyed - the entry carries its own
			// copies (invalid = no slot to release).
			BufferHandle BufferSlot;
			uint32_t    BindlessIndex = 0;
			bool        HasBindlessIndex = false;
			// P-F: the tombstone log write's seq (0 = none). The index is
			// reusable only once the reset tag passed AND every
			// slot's set has applied this seq.
			uint64_t    TombstoneSeq = 0;
		};
		void QueueRetire(RetireEntry&& entry);
		void FreeRetireEntry(const RetireEntry& entry);
		// The Vk objects + the bindless index (under the descriptor log lock).
		void FreeRetireEntryObjects(const RetireEntry& entry);
		// The dev assert every retire-queue access goes through (the game
		// thread is its one owner).
		void AssertRetireQueueOwner(const char* entryPoint) const;

		// The generation-checked handle table: fixed-slot objects + a
		// freelist; a freed slot bumps its generation. Deferred destruction
		// (the retire queue) arrives in P-A3 - P-A1 destroys synchronously.
		template <typename THandle, typename TObject>
		struct Table
		{
			// The handle tables are read by the Loom record workers
			// (GetBufferHandle/GetTextureView during record) while the game
			// thread's extract mutates them (CreateBuffer/DestroyBuffer +
			// streaming creates and the retire-queue frees) - a plain vector
			// reallocation torn by a concurrent Get() is a data race that
			// corrupts the handle and SIGSEGVs in the driver. Readers take
			// the shared lock (readers don't block readers); writers take the
			// exclusive lock (they are infrequent, game-thread-only) - the
			// same shape as the descriptor write log's mutex.
			struct Slot
			{
				std::unique_ptr<TObject> Object;
				uint32_t Generation = 0;
				bool Alive = false;
				// Destroyed by its owner, the release pending in the retire
				// queue (buffers only). Get() treats it as gone;
				// GetResolvableNoLock still resolves it.
				bool Retiring = false;
			};
			std::vector<Slot> Slots;
			std::vector<uint32_t> Free;
			mutable std::shared_mutex Mutex;

			THandle Acquire()
			{
				std::unique_lock lock(Mutex);
				uint32_t index;
				if (!Free.empty())
				{
					index = Free.back();
					Free.pop_back();
				}
				else
				{
					index = (uint32_t)Slots.size();
					Slots.emplace_back();
				}
				Slot& slot = Slots[index];
				slot.Alive = true;
				slot.Retiring = false;
				slot.Object.reset();
				THandle handle;
				handle.Index = index;
				handle.Generation = slot.Generation;
				return handle;
			}

			bool Release(const THandle& handle)
			{
				std::unique_lock lock(Mutex);
				if (handle.Index >= Slots.size() || !Slots[handle.Index].Alive)
					return false;
				if (Slots[handle.Index].Generation != handle.Generation)
					return false;
				Slot& slot = Slots[handle.Index];
				slot.Object.reset();
				slot.Alive = false;
				slot.Retiring = false;
				slot.Generation++;
				Free.push_back(handle.Index);
				return true;
			}

			TObject* Get(const THandle& handle) const
			{
				std::shared_lock lock(Mutex);
				return GetNoLock(handle);
			}

			// Unlocked lookup for callers that already hold the shared lock
			// and need to read the object's fields under it (reading them
			// AFTER Get()'s lock dropped is a use-after-free against a
			// concurrent Release).
			TObject* GetNoLock(const THandle& handle) const
			{
				TObject* object = GetResolvableNoLock(handle);
				return object && !Slots[handle.Index].Retiring ? object : nullptr;
			}

			// The record-side lookup: also resolves a Retiring slot (its
			// object lives until the retire queue releases it, and a
			// pipelined frame's record may still name the handle).
			TObject* GetResolvableNoLock(const THandle& handle) const
			{
				if (handle.Index >= Slots.size() || !Slots[handle.Index].Alive)
					return nullptr;
				if (Slots[handle.Index].Generation != handle.Generation)
					return nullptr;
				return Slots[handle.Index].Object.get();
			}

			// The owner's destroy - marks the live slot Retiring and returns
			// its object (nullptr for an invalid, stale or already-retiring
			// handle). Release frees it.
			TObject* BeginRetire(const THandle& handle)
			{
				std::unique_lock lock(Mutex);
				TObject* object = GetNoLock(handle);
				if (object)
					Slots[handle.Index].Retiring = true;
				return object;
			}

			// Publishes the slot's object AFTER the (slow) caller-side
			// initialization - the write takes the exclusive lock so a
			// concurrent Get() can never observe a half-published slot
			// (the previous raw post-Acquire writes raced the shared Get()
			// reads - a data race that corrupted handle lookups).
			void Publish(const THandle& handle, std::unique_ptr<TObject> obj)
			{
				std::unique_lock lock(Mutex);
				if (handle.Index < Slots.size())
					Slots[handle.Index].Object = std::move(obj);
			}

			void Clear()
			{
				std::unique_lock lock(Mutex);
				for (Slot& slot : Slots)
					slot.Object.reset();
				Slots.clear();
				Free.clear();
			}
		};

		VulkanDevice m_Vulkan;
		bool m_Initialized = false;
		float m_ClearColour[4] = { 0.1f, 0.1f, 0.1f, 1.0f };

		Table<BufferHandle, VulkanBuffer> m_Buffers;
		Table<TextureHandle, VulkanTextureEntry> m_Textures;
		Table<SamplerHandle, VulkanSampler> m_Samplers;
		Table<ShaderHandle, VulkanShaderModule> m_Shaders;
		Table<PipelineHandle, VulkanPipeline> m_Pipelines;

		// The pipeline cache - keyed by (state hash, vertex shader handle,
		// fragment shader handle); a hit returns the cached pipeline
		// (refcounted so create-once/destroy-once callers stay balanced).
		// Entries evict when either shader dies or when
		// InvalidatePipelines names the shader's AssetID.
		struct PipelineCacheEntry
		{
			PipelineHandle Handle;
			uint32_t RefCount = 0;
			// What a shader reload rebuilds it from.
			VulkanPipelineSpec Spec;
			std::string Name;
		};
		struct PipelineCacheKey
		{
			uint64_t StateHash = 0;
			ShaderHandle VertexShader;
			ShaderHandle FragmentShader;

			bool operator==(const PipelineCacheKey& other) const
			{
				return StateHash == other.StateHash && VertexShader.Index == other.VertexShader.Index
					&& VertexShader.Generation == other.VertexShader.Generation
					&& FragmentShader.Index == other.FragmentShader.Index
					&& FragmentShader.Generation == other.FragmentShader.Generation;
			}
		};
		struct PipelineCacheKeyHash
		{
			size_t operator()(const PipelineCacheKey& k) const
			{
				size_t h = std::hash<uint64_t>{}(k.StateHash);
				h ^= std::hash<uint32_t>{}(k.VertexShader.Index) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.VertexShader.Generation) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.FragmentShader.Index) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= std::hash<uint32_t>{}(k.FragmentShader.Generation) + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};
		std::unordered_map<PipelineCacheKey, PipelineCacheEntry, PipelineCacheKeyHash> m_PipelineCache;
		uint64_t HashPipelineStateDesc(const PipelineStateDesc& desc);
		// The cache's shared tail: a hit bumps the refcount, a miss builds
		// the pipeline from `spec` and publishes it.
		PipelineHandle FindOrCreateCachedPipeline(const PipelineCacheKey& key, const VulkanPipelineSpec& spec, const char* debugName);
		void EvictPipelineEntry(const PipelineCacheKey& key);
		void CreatePipelineCache();
		void SavePipelineCache();

		// The on-disk driver pipeline cache
		// (saved/cache/pipeline.bin, gated by
		// r.shader.pipelineCacheFile).
		VkPipelineCache m_VkPipelineCache = VK_NULL_HANDLE;
		Table<CommandListHandle, VulkanCommandList> m_CommandLists;
		Table<SwapchainHandle, VulkanSwapchain> m_Swapchains;

		// P-D: the two fixed samplers Wisp picks from at texture creation
		// (baked with Wisp's exact parameters - single-level filtering).
		VkSampler m_FixedSamplerLinear = VK_NULL_HANDLE;
		VkSampler m_FixedSamplerNearest = VK_NULL_HANDLE;

		// P-F: the tombstone target (a real 1x1 white texture) - freed
		// slots point here until recycled, so no slot's set ever names a
		// destroyed view and no null-handle write is needed.
		VkImage m_NullTextureImage = VK_NULL_HANDLE;
		VmaAllocation m_NullTextureAllocation = VK_NULL_HANDLE;
		VkImageView m_NullTextureView = VK_NULL_HANDLE;
		VkSampler m_NullTextureSampler = VK_NULL_HANDLE;
		// The buffer-side null descriptor (16 bytes).
		VkBuffer m_NullBuffer = VK_NULL_HANDLE;
		VmaAllocation m_NullBufferAllocation = VK_NULL_HANDLE;

		VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;
		VkDescriptorPool m_GlobalSetPool = VK_NULL_HANDLE;
		// One set per frame slot (identical layout); a slot's set is only
		// written when that slot begins recording (its previous
		// submission has retired by then).
		VkDescriptorSet m_GlobalSets[VulkanDevice::TotalFrameSlots] = {};

		// P-F: the write log + per-slot apply bookkeeping. Guarded by
		// m_DescriptorLogMutex (appends happen on the game thread at
		// creation/destruction; the apply + bind run on record threads).
		std::vector<DescriptorWriteLogEntry> m_DescriptorWriteLog;
		uint64_t m_NextDescriptorSeq = 1;
		uint64_t m_LastAppliedSeq[VulkanDevice::TotalFrameSlots] = {};
		// The per-slot expected/activation flags: a window's slots register
		// when the window allocates its slot base (NoteSlotExpected), and the
		// GC floor takes the min over the EXPECTED slots only. The
		// never-expected slots (the never-opened windows) stay out of the
		// floor (a min over every slot never moved: the log grew forever and
		// tombstoned indices never recycled), and an expected slot that
		// hasn't applied yet pins the floor at its seq 0 until its first
		// apply (the re-seed below). A closing window deactivates its slots
		// (DeactivateWindowSlots).
		bool m_SlotExpected[VulkanDevice::TotalFrameSlots] = {};
		// The latest log entry per bindless index (textures: binding 0,
		// buffers: binding 1; Seq 0 = never written), the tombstones of
		// indices pending reuse included. Every append to the log is
		// mirrored here and the GC never drops from it, so a slot that
		// becomes expected mid-session (or is recycled) re-seeds its set
		// from it at its first apply instead of replaying only the log's
		// tail. Guarded by m_DescriptorLogMutex.
		std::vector<DescriptorWriteLogEntry> m_CurrentTextureDescriptors;
		std::vector<DescriptorWriteLogEntry> m_CurrentBufferDescriptors;
		// False until a slot's first apply (and again from NoteSlotExpected):
		// that apply is the full re-seed from the tables above.
		bool m_SlotSeeded[VulkanDevice::TotalFrameSlots] = {};
		std::vector<std::pair<uint32_t, uint64_t>> m_PendingIndexReuse; // (index, tombstone seq)
		mutable std::mutex m_DescriptorLogMutex;

		// The apply-side scratch buffers (reused across applies instead
		// of the old per-slot-per-frame three vector allocations at
		// full-log size). Only touched under m_DescriptorLogMutex.
		std::vector<VkWriteDescriptorSet> m_ApplyScratchWrites;
		std::vector<VkDescriptorImageInfo> m_ApplyScratchImageInfos;
		std::vector<VkDescriptorBufferInfo> m_ApplyScratchBufferInfos;
		// The entries one apply writes (the log's tail, or the re-seed
		// tables).
		std::vector<const DescriptorWriteLogEntry*> m_ApplyScratchEntries;

		std::vector<uint32_t> m_FreeBindlessIndices;
		uint32_t m_NextBindlessIndex = 0;
		// The buffer binding's own free/reuse lists (the two bindings'
		// index spaces are separate).
		std::vector<std::pair<uint32_t, uint64_t>> m_PendingBufferIndexReuse;
		std::vector<uint32_t> m_FreeBindlessBufferIndices;
		uint32_t m_NextBindlessBufferIndex = 0;
		// Game-thread-owned, unlocked by design (AssertRetireQueueOwner).
		std::vector<RetireEntry> m_RetireQueue;
		// The per-slot record-start reset counts (the submit thread's
		// NoteSlotReset bumps them after the command buffers free) - the
		// retire entries tag against them.
		std::atomic<uint64_t> m_SlotResetCount[VulkanDevice::TotalFrameSlots] = {};

		QueueHandle m_GraphicsQueue;
	};

} // namespace rhi

// P-E: the entry points for Platform/Vulkan code to reach the RHI impl
// (replaces the deleted VulkanDeviceRegistry shim). EnsureDevice is a
// no-op once the first window has created the device - callers that may
// run pre-window must go through the subsystem's EnsureDevice(window)
// directly (the helper asserts without a window).
rhi::VulkanRhiDevice& GetVulkanRhiDevice();

inline VulkanDevice& GetVulkanDevice()
{
	return *GetVulkanRhiDevice().GetVulkan();
}

} // namespace TinyEngine
