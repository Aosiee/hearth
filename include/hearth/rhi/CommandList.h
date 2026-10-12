#pragma once

// The command-list surface. Recordable from any thread in contract; today
// it records on the main thread only (recording threads come later). A
// CommandList is bound to the thread whose pool it came from - recording
// from another thread is a dev assert.

#include "TinyEngine/RHI/Types.h"
#include "TinyEngine/RHI/States.h"
#include "TinyEngine/RHI/Format.h"
#include "TinyEngine/RHI/BarrierDesc.h"

#include <cstdint>
#include <span>

namespace TinyEngine {
namespace rhi {

	// One dynamic-rendering attachment set (the backend uses dynamic
	// rendering, so there are no render-pass objects).
	// The render area comes from the first colour attachment's size.
	// The colour side is an ARRAY (MRT for the
	// G-buffer pass); LoadColor still clears every colour attachment with
	// the device's single clear colour (no per-attachment clear values).
	struct AttachmentDesc
	{
		static constexpr uint32_t kMaxColorAttachments = 8;

		TextureHandle Colors[kMaxColorAttachments] = {}; // invalid = absent
		uint32_t ColorCount = 1;
		// Per-attachment clear values (the G-buffer pass clears
		// GB/SceneColor to different defaults than the display target's
		// clear colour). LoadColor = false ignores these.
		float ClearValues[kMaxColorAttachments][4] = {};
		TextureHandle Depth;               // invalid = no depth attachment
		bool   DepthReadOnly = false;
		bool   LoadColor = true;           // true = clear (the per-attachment values above)
		bool   LoadDepth = true;
	};

	class CommandList
	{
	public:
		virtual ~CommandList() = default;

		virtual void Begin() = 0;
		virtual void End() = 0;

		virtual void BeginRendering(std::span<const AttachmentDesc> attachments) = 0;
		virtual void EndRendering() = 0;

		// Dynamic viewport/scissor state (P-D added these when Wisp ported -
		// the renderers kept raw viewport/scissor commands through P-C).
		virtual void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) = 0;
		virtual void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;

		virtual void Barrier(std::span<const BarrierDesc> barriers) = 0;

		virtual void BindPipeline(PipelineHandle pipeline) = 0;
		// Binds the device's global bindless descriptor set at set 0. Must
		// follow BindPipeline (the set layout lives in the pipeline layout).
		virtual void BindGlobalSet() = 0;
		virtual void PushConstants(uint32_t stages, uint32_t offset, const void* data, uint32_t size) = 0;
		virtual void BindVertexBuffer(BufferHandle buffer, uint64_t offset = 0) = 0;
		virtual void BindIndexBuffer(BufferHandle buffer, uint64_t offset = 0) = 0;

		virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1,
			uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
		virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
			uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;

		virtual void Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ) = 0; // plumbed; no consumer yet

		virtual void CopyBufferToBuffer(BufferHandle src, BufferHandle dst,
			uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
		virtual void CopyBufferToTexture(BufferHandle src, TextureHandle dst,
			uint32_t width, uint32_t height) = 0;
		virtual void CopyTextureToBuffer(TextureHandle src, BufferHandle dst,
			uint32_t width, uint32_t height) = 0;

		virtual void BeginLabel(const char* name) = 0;
		virtual void EndLabel() = 0;
		virtual void WriteTimestamp(uint32_t queryIndex) = 0;
	};

} // namespace rhi
} // namespace TinyEngine
