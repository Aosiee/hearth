#include "tepch.h"
#include "Platform/Vulkan/RHI/VulkanRhiDevice.h"
#include "Platform/Vulkan/RHI/DescriptorLogUtil.h"
#include "Platform/Vulkan/RHI/PipelineCacheFile.h"
#include "Platform/Vulkan/Shaders/VulkanShaderReflection.h"
#include "Platform/Vulkan/Device/VulkanContext.h" // the main context's GetCurrentFrameSlot (the retire queue's slot tag)

#include "TinyEngine/Core/Log.h"
#include "TinyEngine/Core/Application.h"
#include "TinyEngine/RHI/RhiDevice.h"
#include "TinyEngine/CVar/EngineCVars.h"
#include <blake3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace TinyEngine {

	rhi::VulkanRhiDevice& GetVulkanRhiDevice()
	{
		Application* app = Application::TryGet();
		TE_CORE_ASSERT(app, "GetVulkanRhiDevice() ran before the Application (and its Engine) exists!");
		RhiDevice& rhi = app->GetEngine().Get<RhiDevice>();
		rhi.EnsureDevice(nullptr); // ASSERTS if called before the first window created the device (the guard)
		return static_cast<rhi::VulkanRhiDevice&>(rhi.Get());
	}

namespace rhi {

	namespace {

		VkShaderStageFlagBits ToVkStage(ShaderStage stage)
		{
			switch (stage)
			{
			case ShaderStage::Vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
			case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
			case ShaderStage::Compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
			}
			return VK_SHADER_STAGE_VERTEX_BIT;
		}

		VkCompareOp ToVkCompareOp(CompareOp op)
		{
			switch (op)
			{
			case CompareOp::Never:          return VK_COMPARE_OP_NEVER;
			case CompareOp::Less:           return VK_COMPARE_OP_LESS;
			case CompareOp::LessOrEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Equal:          return VK_COMPARE_OP_EQUAL;
			case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case CompareOp::Greater:        return VK_COMPARE_OP_GREATER;
			case CompareOp::Always:         return VK_COMPARE_OP_ALWAYS;
			}
			return VK_COMPARE_OP_LESS;
		}

		VkPolygonMode ToVkPolygonMode(PolygonFillMode mode)
		{
			switch (mode)
			{
			case PolygonFillMode::Fill: return VK_POLYGON_MODE_FILL;
			case PolygonFillMode::Line: return VK_POLYGON_MODE_LINE;
			}
			return VK_POLYGON_MODE_FILL;
		}

		VkBlendFactor ToVkBlendFactor(BlendFactor factor)
		{
			switch (factor)
			{
			case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
			case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
			case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
			case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			}
			return VK_BLEND_FACTOR_SRC_ALPHA;
		}

		VkFormat ToVkFormat(Format format)
		{
			switch (format)
			{
			case Format::B8G8R8A8_UNORM:     return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::B8G8R8A8_SRGB:      return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::R8G8B8A8_UNORM:     return VK_FORMAT_R8G8B8A8_UNORM;
			case Format::R8G8B8A8_SRGB:      return VK_FORMAT_R8G8B8A8_SRGB;
			case Format::R8_UNORM:           return VK_FORMAT_R8_UNORM;
			case Format::D32_SFLOAT:         return VK_FORMAT_D32_SFLOAT;
			case Format::R16G16_UNORM:       return VK_FORMAT_R16G16_UNORM;
			case Format::R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
			case Format::R16G16_SFLOAT:      return VK_FORMAT_R16G16_SFLOAT;
			case Format::R32G32B32_SFLOAT:   return VK_FORMAT_R32G32B32_SFLOAT;
			case Format::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
			case Format::R32G32_SFLOAT:      return VK_FORMAT_R32G32_SFLOAT;
			case Format::R32_SFLOAT:         return VK_FORMAT_R32_SFLOAT;
			case Format::Undefined:          return VK_FORMAT_UNDEFINED;
			}
			return VK_FORMAT_UNDEFINED;
		}

		VkImageLayout ToVkLayout(TextureState state)
		{
			switch (state)
			{
			case TextureState::Undefined:    return VK_IMAGE_LAYOUT_UNDEFINED;
			case TextureState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			// The engine's depth images are D32 depth-only (no stencil
			// aspect anywhere), so the depth-only layouts are the ones every
			// scope + tracker uses - P4-C caught the depth-view barriers
			// deriving the STENCIL variants and mismatching.
			case TextureState::DepthWrite:   return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			case TextureState::DepthRead:    return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
			case TextureState::ShaderRead:   return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case TextureState::DepthShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case TextureState::TransferSrc:  return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case TextureState::TransferDst:  return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case TextureState::Present:      return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case TextureState::SwapchainAcquired: return VK_IMAGE_LAYOUT_UNDEFINED;
			}
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}

		// P-A2: the one place states map to (stage, access) pairs. The masks
		// cover the access the state implies; every barrier is emitted via
		// synchronization2 (VkDependencyInfo), never vkCmdPipelineBarrier.
		VkPipelineStageFlags2 ToVkStageMask(TextureState state)
		{
			switch (state)
			{
			// TOP_OF_PIPE (not NONE) is the tree's proven first-use source
			// stage; an empty stage here would give a barrier with an empty
			// first synchronization scope.
			case TextureState::Undefined:    return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
			case TextureState::RenderTarget: return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			case TextureState::DepthWrite:   return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			// DepthRead is the shared shadow atlases' rest state, SAMPLED
			// by the lighting fragment shaders in DEPTH_READ_ONLY_OPTIMAL
			// (the layout their bindless descriptors are written in) - the
			// fragment-shader stage + sampled read join the depth-test
			// read, so a cast -> read barrier makes the cast's writes
			// visible to the samples and a read -> cast barrier orders
			// those samples before the next cast (the WAR edge).
			case TextureState::DepthRead:    return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			case TextureState::ShaderRead:   return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			case TextureState::DepthShaderRead: return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			case TextureState::TransferSrc:  return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			case TextureState::TransferDst:  return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			case TextureState::Present:      return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
			// The acquire semaphore's wait stage
			// (VulkanSwapchain::SubmitAndPresent's pWaitDstStageMask) -
			// the first barrier's source scope must include it so the
			// layout transition chains after the wait (TOP_OF_PIPE did
			// not: the syncval WAR against vkAcquireNextImageKHR).
			// Change the two together.
			case TextureState::SwapchainAcquired: return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			}
			return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		}

		VkAccessFlags2 ToVkAccessMask(TextureState state)
		{
			switch (state)
			{
			case TextureState::Undefined:    return VK_ACCESS_2_NONE;
			case TextureState::RenderTarget: return VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			case TextureState::DepthWrite:   return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			case TextureState::DepthRead:    return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			case TextureState::ShaderRead:   return VK_ACCESS_2_SHADER_READ_BIT;
			case TextureState::DepthShaderRead: return VK_ACCESS_2_SHADER_READ_BIT;
			case TextureState::TransferSrc:  return VK_ACCESS_2_TRANSFER_READ_BIT;
			case TextureState::TransferDst:  return VK_ACCESS_2_TRANSFER_WRITE_BIT;
			case TextureState::Present:      return VK_ACCESS_2_NONE;
			case TextureState::SwapchainAcquired: return VK_ACCESS_2_NONE;
			}
			return VK_ACCESS_2_NONE;
		}

		VkPipelineStageFlags2 ToVkStageMask(BufferState state)
		{
			switch (state)
			{
			case BufferState::Undefined:    return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
			case BufferState::VertexIndex:  return VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
			case BufferState::Uniform:      return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			case BufferState::Storage:      return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			case BufferState::TransferSrc:  return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			case BufferState::TransferDst:  return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			case BufferState::Indirect:     return VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
			}
			return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		}

		VkAccessFlags2 ToVkAccessMask(BufferState state)
		{
			switch (state)
			{
			case BufferState::Undefined:    return VK_ACCESS_2_NONE;
			case BufferState::VertexIndex:  return VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
			case BufferState::Uniform:      return VK_ACCESS_2_UNIFORM_READ_BIT;
			case BufferState::Storage:      return VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
			case BufferState::TransferSrc:  return VK_ACCESS_2_TRANSFER_READ_BIT;
			case BufferState::TransferDst:  return VK_ACCESS_2_TRANSFER_WRITE_BIT;
			case BufferState::Indirect:     return VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
			}
			return VK_ACCESS_2_NONE;
		}

		constexpr const char* kPipelineCachePath = "saved/cache/pipeline.bin";

		PipelineCacheIdentity QueryPipelineCacheIdentity(VkPhysicalDevice physicalDevice)
		{
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(physicalDevice, &props);
			PipelineCacheIdentity identity;
			identity.VendorId = props.vendorID;
			identity.DeviceId = props.deviceID;
			memcpy(identity.CacheUuid, props.pipelineCacheUUID, sizeof(identity.CacheUuid));
			return identity;
		}

	} // namespace

	// ---- VulkanSampler ----------------------------------------------------------
	// The standalone sampler (the existing VulkanTexture2D bakes its own
	// internally; this is the RHI's general form, consumed by the fixed
	// sampler table in P-A3).

	void VulkanSampler::Init(VulkanDevice& device, const SamplerDesc& desc, const std::string& debugName)
	{
		m_Device = &device;

		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = desc.Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		info.minFilter = desc.Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		info.mipmapMode = desc.Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.addressModeU = desc.Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = info.addressModeU;
		info.addressModeW = info.addressModeU;
		info.anisotropyEnable = desc.Anisotropy && device.IsSamplerAnisotropySupported();
		info.maxAnisotropy = info.anisotropyEnable ? 16.0f : 1.0f;
		info.minLod = 0.0f;
		info.maxLod = VK_LOD_CLAMP_NONE;

		VulkanCheckResult(vkCreateSampler(device.GetDevice(), &info, nullptr, &m_Sampler), "vkCreateSampler", __FILE__, __LINE__);
		if (!debugName.empty())
			device.SetObjectName(VK_OBJECT_TYPE_SAMPLER, (uint64_t)m_Sampler, debugName);
	}

	void VulkanSampler::Shutdown()
	{
		if (m_Sampler && m_Device)
			vkDestroySampler(m_Device->GetDevice(), m_Sampler, nullptr);
		m_Sampler = VK_NULL_HANDLE;
		m_Device = nullptr;
	}

	VkSampler VulkanSampler::GetHandle() const { return m_Sampler; }

	// ---- VulkanCommandList ------------------------------------------------------
	// Wraps one VkCommandBuffer and resolves every handle through the owning
	// device. Command buffers come from the device's (frame, thread)-keyed pools;
	// the list carries the frame slot of the pool (it replaced the old thread-local
	// record index).

	VulkanCommandList::VulkanCommandList(VulkanRhiDevice* device, VkCommandBuffer commandBuffer, uint32_t frameSlot)
		: CommandList(frameSlot), m_Device(device), m_CommandBuffer(commandBuffer)
	{
		TE_CORE_ASSERT(frameSlot < VulkanDevice::TotalFrameSlots, "a command list constructed with an out-of-range frame slot");
	}

	void VulkanCommandList::Begin()
	{
		VkCommandBufferBeginInfo info{};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VulkanCheckResult(vkBeginCommandBuffer(m_CommandBuffer, &info), "vkBeginCommandBuffer", __FILE__, __LINE__);
	}

	void VulkanCommandList::End()
	{
		VulkanCheckResult(vkEndCommandBuffer(m_CommandBuffer), "vkEndCommandBuffer", __FILE__, __LINE__);
	}

	void VulkanCommandList::BeginRendering(std::span<const AttachmentDesc> attachments)
	{
		VkRenderingInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		VkRenderingAttachmentInfo colourAttachments[AttachmentDesc::kMaxColorAttachments]{};
		uint32_t colourCount = 0;
		VkRenderingAttachmentInfo depthAttachment{};
		bool hasDepth = false;
		VkExtent2D renderArea{};

		for (const AttachmentDesc& attachment : attachments)
		{
			for (uint32_t i = 0; i < attachment.ColorCount; ++i)
			{
				if (!IsValid(attachment.Colors[i]))
					continue;
				VkRenderingAttachmentInfo& colourAttachment = colourAttachments[colourCount];
				colourAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				colourAttachment.imageView = m_Device->GetTextureView(attachment.Colors[i]);
				colourAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				colourAttachment.loadOp = attachment.LoadColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				colourAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				// The per-attachment clear value (the single device clear
				// colour stays as the fallback for callers that don't set
				// one - all-zero is the default).
				colourAttachment.clearValue.color = { { attachment.ClearValues[i][0], attachment.ClearValues[i][1],
					attachment.ClearValues[i][2], attachment.ClearValues[i][3] } };
				colourCount++;
				if (renderArea.width == 0)
					renderArea = m_Device->GetTextureSize(attachment.Colors[i]);
			}
			if (IsValid(attachment.Depth))
			{
				hasDepth = true;
				depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				depthAttachment.imageView = m_Device->GetTextureView(attachment.Depth);
				depthAttachment.imageLayout = attachment.DepthReadOnly
					? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
					: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depthAttachment.loadOp = attachment.LoadDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				depthAttachment.clearValue.depthStencil = { 1.0f, 0 };
			}
		}
		renderingInfo.colorAttachmentCount = colourCount;
		renderingInfo.pColorAttachments = colourAttachments;
		renderingInfo.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;
		// The render area comes from the first colour attachment's size.
		renderingInfo.renderArea.extent = renderArea;
		vkCmdBeginRendering(m_CommandBuffer, &renderingInfo);
	}

	void VulkanCommandList::EndRendering()
	{
		vkCmdEndRendering(m_CommandBuffer);
	}

	void VulkanCommandList::Barrier(std::span<const BarrierDesc> barriers)
	{
		// P-A2: synchronization2 only - every barrier becomes a
		// VkImageMemoryBarrier2/VkBufferMemoryBarrier2 inside one
		// VkDependencyInfo. State-to-(stage, access, layout) lives solely
		// in the anonymous-namespace tables above.
		std::vector<VkImageMemoryBarrier2> imageBarriers;
		std::vector<VkBufferMemoryBarrier2> bufferBarriers;

		for (const BarrierDesc& barrier : barriers)
		{
			for (uint32_t i = 0; i < barrier.TextureCount; i++)
			{
				const TextureBarrier& t = barrier.Textures[i];
				bool depth = t.From == TextureState::DepthWrite || t.From == TextureState::DepthRead
					|| t.From == TextureState::DepthShaderRead
					|| t.To == TextureState::DepthWrite || t.To == TextureState::DepthRead
					|| t.To == TextureState::DepthShaderRead;
				VkImageMemoryBarrier2 b{};
				b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				b.image = m_Device->GetTextureImage(t.Texture);
				b.oldLayout = ToVkLayout(t.From);
				b.newLayout = ToVkLayout(t.To);
				b.srcStageMask = ToVkStageMask(t.From);
				b.srcAccessMask = ToVkAccessMask(t.From);
				b.dstStageMask = ToVkStageMask(t.To);
				b.dstAccessMask = ToVkAccessMask(t.To);
				b.subresourceRange = {
					depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
					t.BaseMip, t.MipCount, t.BaseLayer, t.LayerCount
				};
				imageBarriers.push_back(b);
			}
			for (uint32_t i = 0; i < barrier.BufferCount; i++)
			{
				const BufferBarrier& t = barrier.Buffers[i];
				VkBufferMemoryBarrier2 b{};
				b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
				b.buffer = m_Device->GetBufferHandle(t.Buffer);
				b.srcStageMask = ToVkStageMask(t.From);
				b.srcAccessMask = ToVkAccessMask(t.From);
				b.dstStageMask = ToVkStageMask(t.To);
				b.dstAccessMask = ToVkAccessMask(t.To);
				b.offset = t.Offset;
				b.size = t.Size;
				bufferBarriers.push_back(b);
			}
		}

		if (!imageBarriers.empty() || !bufferBarriers.empty())
		{
			VkDependencyInfo dependency{};
			dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dependency.imageMemoryBarrierCount = (uint32_t)imageBarriers.size();
			dependency.pImageMemoryBarriers = imageBarriers.data();
			dependency.bufferMemoryBarrierCount = (uint32_t)bufferBarriers.size();
			dependency.pBufferMemoryBarriers = bufferBarriers.data();
			vkCmdPipelineBarrier2(m_CommandBuffer, &dependency);
		}
	}

	void VulkanCommandList::BindPipeline(PipelineHandle pipeline)
	{
		m_BoundLayout = m_Device->GetPipelineLayout(pipeline);
		vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Device->GetPipelineHandle(pipeline));
	}

	void VulkanCommandList::BindGlobalSet()
	{
		// The global bindless set at set 0; the layout comes from the bound
		// pipeline's layout, so BindPipeline must have run first.
		// The list's record-side frame slot picks the slot's SET. The set
		// itself is applied ONCE per slot by the submit thread BEFORE the
		// record fan-out (ProcessSubmittedFrame) - applying it here would
		// update the set concurrently with the sibling workers' binds and
		// draws on the same set (a host-access race the driver's bind path
		// then crashes on).
		TE_CORE_ASSERT(m_BoundLayout != VK_NULL_HANDLE, "BindGlobalSet called before BindPipeline");
		const uint32_t slot = FrameSlot();
		TE_CORE_ASSERT(slot < VulkanDevice::TotalFrameSlots, "BindGlobalSet with an out-of-range record frame slot");
		VkDescriptorSet set = m_Device->GetGlobalDescriptorSet(slot);
		vkCmdBindDescriptorSets(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_BoundLayout, 0, 1, &set, 0, nullptr);
	}

	void VulkanCommandList::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth)
	{
		VkViewport viewport{};
		viewport.x = x;
		viewport.y = y;
		viewport.width = width;
		viewport.height = height;
		viewport.minDepth = minDepth;
		viewport.maxDepth = maxDepth;
		vkCmdSetViewport(m_CommandBuffer, 0, 1, &viewport);
	}

	void VulkanCommandList::SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
	{
		VkRect2D scissor{};
		scissor.offset = { x, y };
		scissor.extent = { width, height };
		vkCmdSetScissor(m_CommandBuffer, 0, 1, &scissor);
	}

	void VulkanCommandList::SetDepthBias(float constantFactor, float clamp, float slopeFactor)
	{
		vkCmdSetDepthBias(m_CommandBuffer, constantFactor, clamp, slopeFactor);
	}

	void VulkanCommandList::PushConstants(uint32_t stages, uint32_t offset, const void* data, uint32_t size)
	{
		vkCmdPushConstants(m_CommandBuffer, m_BoundLayout, (VkShaderStageFlags)stages, offset, size, data);
	}

	void VulkanCommandList::BindVertexBuffer(BufferHandle buffer, uint64_t offset)
	{
		VkBuffer handle = m_Device->GetBufferHandle(buffer);
		VkDeviceSize offsets[] = { offset };
		vkCmdBindVertexBuffers(m_CommandBuffer, 0, 1, &handle, offsets);
	}

	void VulkanCommandList::BindIndexBuffer(BufferHandle buffer, uint64_t offset)
	{
		vkCmdBindIndexBuffer(m_CommandBuffer, m_Device->GetBufferHandle(buffer), offset, VK_INDEX_TYPE_UINT32);
	}

	void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
	{
		vkCmdDraw(m_CommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void VulkanCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		vkCmdDrawIndexed(m_CommandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	void VulkanCommandList::Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ)
	{
		vkCmdDispatch(m_CommandBuffer, groupX, groupY, groupZ);
	}

	void VulkanCommandList::CopyBufferToBuffer(BufferHandle src, BufferHandle dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
	{
		VkBufferCopy region{ srcOffset, dstOffset, size };
		vkCmdCopyBuffer(m_CommandBuffer, m_Device->GetBufferHandle(src), m_Device->GetBufferHandle(dst), 1, &region);
	}

	void VulkanCommandList::CopyBufferToTexture(BufferHandle src, TextureHandle dst, uint32_t width, uint32_t height)
	{
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { width, height, 1 };
		vkCmdCopyBufferToImage(m_CommandBuffer, m_Device->GetBufferHandle(src),
			m_Device->GetTextureImage(dst), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	void VulkanCommandList::CopyTextureToBuffer(TextureHandle src, BufferHandle dst, uint32_t width, uint32_t height)
	{
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { width, height, 1 };
		vkCmdCopyImageToBuffer(m_CommandBuffer, m_Device->GetTextureImage(src),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Device->GetBufferHandle(dst), 1, &region);
	}

	void VulkanCommandList::BeginLabel(const char* name, bool timed)
	{
		m_Device->GetVulkan()->CmdBeginDebugLabel(m_CommandBuffer, FrameSlot(), name, nullptr, timed);
	}

	void VulkanCommandList::EndLabel()
	{
		m_Device->GetVulkan()->CmdEndDebugLabel(m_CommandBuffer, FrameSlot());
	}

	void VulkanCommandList::WriteTimestamp(uint32_t queryIndex)
	{
		// The device's timestamp pools are frame-indexed; the list's own
		// slot is the pool its frame reads back. The old form always wrote
		// frame 0's pool, so a non-zero slot's labels would land in a pool
		// no one reads. Every list carries a slot, so the old "outside a
		// record job, fall back to pool 0" branch is gone.
		vkCmdWriteTimestamp(m_CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			m_Device->GetVulkan()->GetTimestampPool(FrameSlot()), queryIndex);
	}

	VkCommandBuffer VulkanCommandList::GetCommandBuffer() const { return m_CommandBuffer; }

	// ---- VulkanRhiDevice ------------------------------------------------------

	VulkanRhiDevice::VulkanRhiDevice() = default;

	VulkanRhiDevice::~VulkanRhiDevice()
	{
		// The subsystem idles + destroys the device before this object dies;
		// the handle tables are cleared defensively here (P-A1 synchronous
		// destroy makes them empty already).
		m_Swapchains.Clear();
		m_CommandLists.Clear();
		m_Pipelines.Clear();
		m_Shaders.Clear();
		m_Samplers.Clear();
		m_Textures.Clear();
		m_Buffers.Clear();
	}

	void VulkanRhiDevice::EnsureDevice(GLFWwindow* windowForExtensionQuery)
	{
		if (m_Initialized)
			return;
		// The Offscreen mode passes no window - the device is created
		// without the surface extensions; the windowed path's assert is
		// dropped so the null window flows through (the instance-extension
		// query skips on null).
		m_Vulkan.Init(windowForExtensionQuery);
		m_Initialized = true;
		m_GraphicsQueue.Index = 0;
		m_GraphicsQueue.Generation = 0;
		CreateGlobalBindlessSet();
		CreateFixedSamplers();
		CreateNullTexture();
		CreateNullBuffer();
		CreatePipelineCache();
	}

	// The driver pipeline cache (VkPipelineCache), persisted to
	// saved/cache/pipeline.bin when r.shader.pipelineCacheFile is on. A
	// cache is disposable, so no file state may stop a launch - a rejected
	// file is a cold start, and a blob the driver refuses (NVIDIA:
	// VK_ERROR_UNKNOWN on a damaged body) too.
	void VulkanRhiDevice::CreatePipelineCache()
	{
		VkPipelineCacheCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		std::vector<uint8_t> cached;
		if (TinyEngine::cvars::r_shader_pipelineCacheFile.Get())
		{
			std::ifstream file(kPipelineCachePath, std::ios::binary);
			if (file.is_open())
			{
				cached.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
				const uint8_t* payload = nullptr;
				size_t payloadSize = 0;
				const PipelineCacheFileStatus status = ParsePipelineCacheFile(cached,
					QueryPipelineCacheIdentity(m_Vulkan.GetPhysicalDevice()), payload, payloadSize);
				if (status == PipelineCacheFileStatus::Ok)
				{
					info.initialDataSize = payloadSize;
					info.pInitialData = payload;
				}
				else
					TE_CORE_WARN(LogVulkan, "Pipeline cache: ignoring {0} ({1}), cold start.",
						kPipelineCachePath, PipelineCacheFileStatusName(status));
			}
		}
		VkResult result = vkCreatePipelineCache(m_Vulkan.GetDevice(), &info, nullptr, &m_VkPipelineCache);
		if (result != VK_SUCCESS && info.initialDataSize != 0)
		{
			TE_CORE_WARN(LogVulkan, "Pipeline cache: the driver refused {0} (VkResult {1}), cold start.",
				kPipelineCachePath, (int)result);
			info.initialDataSize = 0;
			info.pInitialData = nullptr;
			result = vkCreatePipelineCache(m_Vulkan.GetDevice(), &info, nullptr, &m_VkPipelineCache);
		}
		VK_CHECK(result);
	}

	void VulkanRhiDevice::SavePipelineCache()
	{
		if (m_VkPipelineCache == VK_NULL_HANDLE)
			return;
		if (!TinyEngine::cvars::r_shader_pipelineCacheFile.Get())
		{
			vkDestroyPipelineCache(m_Vulkan.GetDevice(), m_VkPipelineCache, nullptr);
			m_VkPipelineCache = VK_NULL_HANDLE;
			return;
		}
		size_t size = 0;
		VK_CHECK(vkGetPipelineCacheData(m_Vulkan.GetDevice(), m_VkPipelineCache, &size, nullptr));
		std::vector<uint8_t> blob(size);
		if (size > 0)
			VK_CHECK(vkGetPipelineCacheData(m_Vulkan.GetDevice(), m_VkPipelineCache, &size, blob.data()));
		vkDestroyPipelineCache(m_Vulkan.GetDevice(), m_VkPipelineCache, nullptr);
		m_VkPipelineCache = VK_NULL_HANDLE;
		if (!WritePipelineCacheFileAtomic(kPipelineCachePath,
			BuildPipelineCacheFile(blob.data(), size, QueryPipelineCacheIdentity(m_Vulkan.GetPhysicalDevice()))))
			TE_CORE_WARN(LogVulkan, "Pipeline cache: could not write {0}, the next launch is a cold start.", kPipelineCachePath);
	}

	// The two fixed samplers Wisp picks from. Baked with Wisp's EXACT
	// parameters - single-level filtering: the nearest sampler serves the
	// font atlas, the linear one the image widgets; both CLAMP and clamp
	// maxLod to 0 so a (conceptually) single-level image can never pick a
	// downsampled level.
	void VulkanRhiDevice::CreateFixedSamplers()
	{
		{
			VkSamplerCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.magFilter = VK_FILTER_LINEAR;
			info.minFilter = VK_FILTER_LINEAR;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.minLod = 0.0f;
			info.maxLod = 0.0f;
			VulkanCheckResult(vkCreateSampler(m_Vulkan.GetDevice(), &info, nullptr, &m_FixedSamplerLinear),
				"vkCreateSampler", __FILE__, __LINE__);
			m_Vulkan.SetObjectName(VK_OBJECT_TYPE_SAMPLER, (uint64_t)m_FixedSamplerLinear, "Fixed Sampler (linear, clamp)");
		}
		{
			VkSamplerCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.magFilter = VK_FILTER_NEAREST;
			info.minFilter = VK_FILTER_NEAREST;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			info.minLod = 0.0f;
			info.maxLod = 0.0f;
			VulkanCheckResult(vkCreateSampler(m_Vulkan.GetDevice(), &info, nullptr, &m_FixedSamplerNearest),
				"vkCreateSampler", __FILE__, __LINE__);
			m_Vulkan.SetObjectName(VK_OBJECT_TYPE_SAMPLER, (uint64_t)m_FixedSamplerNearest, "Fixed Sampler (nearest, clamp)");
		}
	}

	// P-F: the 1x1 white texture every tombstone write points a freed slot
	// at. Always a REAL descriptor (no null-handle write): a slot whose
	// view was destroyed can never be named again, and any code that
	// samples a freed index gets a harmless white texel instead of
	// undefined garbage.
	void VulkanRhiDevice::CreateNullTexture()
	{
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.extent = { 1, 1, 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VulkanCheckResult(vmaCreateImage(m_Vulkan.GetAllocator(), &imageInfo, &allocInfo,
			&m_NullTextureImage, &m_NullTextureAllocation, nullptr), "vmaCreateImage", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)m_NullTextureImage, "Bindless Null Texture");

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = m_NullTextureImage;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VulkanCheckResult(vkCreateImageView(m_Vulkan.GetDevice(), &viewInfo, nullptr, &m_NullTextureView),
			"vkCreateImageView", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)m_NullTextureView, "Bindless Null Texture View");

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_NEAREST;
		samplerInfo.minFilter = VK_FILTER_NEAREST;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;
		VulkanCheckResult(vkCreateSampler(m_Vulkan.GetDevice(), &samplerInfo, nullptr, &m_NullTextureSampler),
			"vkCreateSampler", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_SAMPLER, (uint64_t)m_NullTextureSampler, "Bindless Null Texture Sampler");

		// Fill the texel white on the transient pool (device init has no
		// frame machinery to defer through - the waitIdle here is fine).
		{
			VkCommandBuffer cmd = m_Vulkan.BeginSingleTimeCommands();

			VkImageMemoryBarrier2 toDst{};
			toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
			toDst.srcAccessMask = VK_ACCESS_2_NONE;
			toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toDst.image = m_NullTextureImage;
			toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkDependencyInfo toDstDep{};
			toDstDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			toDstDep.imageMemoryBarrierCount = 1;
			toDstDep.pImageMemoryBarriers = &toDst;
			vkCmdPipelineBarrier2(cmd, &toDstDep);

			const VkClearColorValue white{ { 1.0f, 1.0f, 1.0f, 1.0f } };
			const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdClearColorImage(cmd, m_NullTextureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

			VkImageMemoryBarrier2 toRead{};
			toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			toRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toRead.image = m_NullTextureImage;
			toRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkDependencyInfo toReadDep{};
			toReadDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			toReadDep.imageMemoryBarrierCount = 1;
			toReadDep.pImageMemoryBarriers = &toRead;
			vkCmdPipelineBarrier2(cmd, &toReadDep);

			m_Vulkan.EndSingleTimeCommands(cmd);
		}

		// The slot 0 is the reserved "none" sentinel - the shaders'
		// 0-as-none gates (the sky/LUT indices) sample the null texture.
		// The allocator starts at 1 so no real texture can ever land on
		// the sentinel.
		WriteTextureDescriptor(0, m_NullTextureView, m_NullTextureSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		m_NextBindlessIndex = 1;
	}

	void VulkanRhiDevice::DestroyNullTexture()
	{
		// After DestroyGlobalBindlessSet: no set references the view.
		if (m_NullTextureSampler != VK_NULL_HANDLE)
			vkDestroySampler(m_Vulkan.GetDevice(), m_NullTextureSampler, nullptr);
		if (m_NullTextureView != VK_NULL_HANDLE)
			vkDestroyImageView(m_Vulkan.GetDevice(), m_NullTextureView, nullptr);
		if (m_NullTextureImage != VK_NULL_HANDLE)
			vmaDestroyImage(m_Vulkan.GetAllocator(), m_NullTextureImage, m_NullTextureAllocation);
		m_NullTextureSampler = VK_NULL_HANDLE;
		m_NullTextureView = VK_NULL_HANDLE;
		m_NullTextureImage = VK_NULL_HANDLE;
		m_NullTextureAllocation = VK_NULL_HANDLE;
	}

	void VulkanRhiDevice::CreateNullBuffer()
	{
		// The buffer-side tombstone target (16 bytes; storage-buffer
		// descriptors need a real, live buffer).
		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = 16;
		bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VulkanCheckResult(vmaCreateBuffer(m_Vulkan.GetAllocator(), &bufferInfo, &allocInfo,
			&m_NullBuffer, &m_NullBufferAllocation, nullptr), "vmaCreateBuffer", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_BUFFER, (uint64_t)m_NullBuffer, "Bindless Null Buffer");
	}

	void VulkanRhiDevice::DestroyNullBuffer()
	{
		if (m_NullBuffer != VK_NULL_HANDLE)
			vmaDestroyBuffer(m_Vulkan.GetAllocator(), m_NullBuffer, m_NullBufferAllocation);
		m_NullBuffer = VK_NULL_HANDLE;
		m_NullBufferAllocation = VK_NULL_HANDLE;
	}

	void VulkanRhiDevice::ShutdownDevice()
	{
		if (!m_Initialized)
			return;

		// The cache entries are referenced by the table slots - the sweep
		// below owns their destruction, so the cache map itself just
		// clears (the refcounts die with it).
		m_PipelineCache.clear();

		// With no submits left the timeline never advances again: idle the
		// device, drain everything queued so far, queue every still-live
		// non-texture resource (the wrappers have no freeing destructors -
		// only VulkanTexture2D/VulkanSwapchain release themselves through
		// Clear), clear the handle tables, drain again, then hand the (now
		// clean) device over to VulkanDevice::Shutdown.
		vkDeviceWaitIdle(m_Vulkan.GetDevice());
		DrainRetireQueueUnconditional();
		for (const auto& slot : m_Pipelines.Slots)
		{
			if (slot.Alive && slot.Object)
				QueuePipelineForDestruction(slot.Object->GetHandle(), slot.Object->GetLayout());
		}
		for (const auto& slot : m_Shaders.Slots)
		{
			if (slot.Alive && slot.Object)
				QueueShaderForDestruction(slot.Object->GetHandle());
		}
		for (const auto& slot : m_Samplers.Slots)
		{
			if (slot.Alive && slot.Object)
				QueueSamplerForDestruction(slot.Object->GetHandle());
		}
		// A Retiring buffer's entry was freed (and its slot released) by the
		// drain above; the check keeps a straggler from being queued twice.
		for (const auto& slot : m_Buffers.Slots)
		{
			if (slot.Alive && slot.Object && !slot.Retiring)
				QueueBufferForDestruction(slot.Object->GetHandle(), slot.Object->GetAllocation());
		}
		for (const auto& slot : m_Textures.Slots)
		{
			if (slot.Alive && slot.Object && slot.Object->IsWrapped() && slot.Object->Wrapped->GetView() != VK_NULL_HANDLE)
				QueueTextureForDestruction(VK_NULL_HANDLE, VK_NULL_HANDLE, slot.Object->Wrapped->GetView(), VK_NULL_HANDLE, slot.Object->Wrapped->GetBindlessIndex());
		}
		m_Swapchains.Clear();
		m_CommandLists.Clear();
		m_Pipelines.Clear();
		m_Shaders.Clear();
		m_Samplers.Clear();
		m_Textures.Clear();
		m_Buffers.Clear();
		DrainRetireQueueUnconditional();
		DestroyGlobalBindlessSet();
		DestroyNullBuffer();
		DestroyNullTexture();
		// The fixed samplers are device-owned (never in a retire entry).
		if (m_FixedSamplerLinear != VK_NULL_HANDLE)
			vkDestroySampler(m_Vulkan.GetDevice(), m_FixedSamplerLinear, nullptr);
		if (m_FixedSamplerNearest != VK_NULL_HANDLE)
			vkDestroySampler(m_Vulkan.GetDevice(), m_FixedSamplerNearest, nullptr);
		m_FixedSamplerLinear = VK_NULL_HANDLE;
		m_FixedSamplerNearest = VK_NULL_HANDLE;
		// Persist the driver pipeline cache before the device dies (the
		// blob comes from the driver itself).
		SavePipelineCache();
		m_Vulkan.Shutdown();
		m_Initialized = false;
	}

	// ---- P-A3: the global bindless set + the retire queue -----------------------

	void VulkanRhiDevice::CreateGlobalBindlessSet()
	{
		VkPhysicalDeviceDescriptorIndexingProperties indexingProperties{};
		indexingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
		VkPhysicalDeviceProperties2 properties2{};
		properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties2.pNext = &indexingProperties;
		vkGetPhysicalDeviceProperties2(m_Vulkan.GetPhysicalDevice(), &properties2);

		TE_CORE_ASSERT(indexingProperties.maxDescriptorSetUpdateAfterBindSampledImages >= kBindlessTextures,
			"the device's update-after-bind sampled-image capacity is below the bindless texture array");
		TE_CORE_ASSERT(indexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages >= kBindlessTextures,
			"the device's per-stage update-after-bind sampled-image capacity is below the bindless texture array");
		TE_CORE_ASSERT(indexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffers >= kBindlessBuffers,
			"the device's update-after-bind storage-buffer capacity is below the bindless buffer array");
		TE_CORE_ASSERT(indexingProperties.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= kBindlessBuffers,
			"the device's per-stage update-after-bind storage-buffer capacity is below the bindless buffer array");

		// PARTIALLY_BOUND | UPDATE_AFTER_BIND; deliberately no
		// VARIABLE_DESCRIPTOR_COUNT (one binding per set, and the full
		// capacity is allocated anyway - runtimeDescriptorArray sizes the
		// shader's [] from descriptorCount). ALL_GRAPHICS: no compute
		// pipelines exist yet. P-F: UPDATE_UNUSED_WHILE_PENDING is the
		// safety net for the tombstone rewrites (a freed slot is re-pointed
		// at the null texture while the set may still be pending).
		VkDescriptorBindingFlags flags[] = {
			VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
			VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
		};
		VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
		flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		flagsInfo.bindingCount = 2;
		flagsInfo.pBindingFlags = flags;

		VkDescriptorSetLayoutBinding bindings[2] = {};
		bindings[0].binding = 0;
		bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[0].descriptorCount = kBindlessTextures;
		bindings[0].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
		bindings[1].binding = 1;
		bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[1].descriptorCount = kBindlessBuffers;
		bindings[1].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.pNext = &flagsInfo;
		// The layout too must carry the update-after-bind flag for the
		// UPDATE_AFTER_BIND binding flags to be legal
		// (VUID-VkDescriptorSetLayoutCreateInfo-flags-03000).
		layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		layoutInfo.bindingCount = 2;
		layoutInfo.pBindings = bindings;

		VulkanCheckResult(vkCreateDescriptorSetLayout(m_Vulkan.GetDevice(), &layoutInfo, nullptr, &m_GlobalSetLayout),
			"vkCreateDescriptorSetLayout", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, (uint64_t)m_GlobalSetLayout, "Global Bindless Set Layout");

		VkDescriptorPoolSize poolSizes[2] = {
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBindlessTextures * VulkanDevice::TotalFrameSlots },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindlessBuffers * VulkanDevice::TotalFrameSlots },
		};
		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
		// P-F: one set per frame slot (TotalFrameSlots identical sets; a
		// slot's set is only written when that slot begins recording).
		poolInfo.maxSets = VulkanDevice::TotalFrameSlots;
		poolInfo.poolSizeCount = 2;
		poolInfo.pPoolSizes = poolSizes;

		VulkanCheckResult(vkCreateDescriptorPool(m_Vulkan.GetDevice(), &poolInfo, nullptr, &m_GlobalSetPool),
			"vkCreateDescriptorPool", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, (uint64_t)m_GlobalSetPool, "Global Bindless Set Pool");

		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = m_GlobalSetPool;
		allocInfo.descriptorSetCount = VulkanDevice::TotalFrameSlots;
		VkDescriptorSetLayout layouts[VulkanDevice::TotalFrameSlots];
		for (VkDescriptorSetLayout& layout : layouts)
			layout = m_GlobalSetLayout;
		allocInfo.pSetLayouts = layouts;

		VulkanCheckResult(vkAllocateDescriptorSets(m_Vulkan.GetDevice(), &allocInfo, m_GlobalSets),
			"vkAllocateDescriptorSets", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, (uint64_t)m_GlobalSets[0], "Global Bindless Set");
	}

	void VulkanRhiDevice::DestroyGlobalBindlessSet()
	{
		// The sets are freed with the pool (no FREE_DESCRIPTOR_SET flag).
		if (m_GlobalSetPool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(m_Vulkan.GetDevice(), m_GlobalSetPool, nullptr);
		if (m_GlobalSetLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(m_Vulkan.GetDevice(), m_GlobalSetLayout, nullptr);
		m_GlobalSetPool = VK_NULL_HANDLE;
		m_GlobalSetLayout = VK_NULL_HANDLE;
		for (VkDescriptorSet& set : m_GlobalSets)
			set = VK_NULL_HANDLE;
		m_DescriptorWriteLog.clear();
		m_CurrentTextureDescriptors.clear();
		m_CurrentBufferDescriptors.clear();
		m_PendingIndexReuse.clear();
	}

	uint32_t VulkanRhiDevice::AllocateBindlessTextureIndex()
	{
		std::lock_guard lock(m_DescriptorLogMutex);

		uint32_t index;
		if (!m_FreeBindlessIndices.empty())
		{
			index = m_FreeBindlessIndices.back();
			m_FreeBindlessIndices.pop_back();

			// A recycled index's old texture entries (its tombstone, and
			// anything else) are superseded by the new owner's write - the
			// buffer binding's entries at the same number stay.
			DropSupersededEntries(m_DescriptorWriteLog, index, 0);
		}
		else
		{
			TE_CORE_ASSERT(m_NextBindlessIndex < kBindlessTextures, "bindless texture array exhausted");
			index = m_NextBindlessIndex++;
		}
		return index;
	}

	void VulkanRhiDevice::WriteTextureDescriptor(uint32_t index, VkImageView imageView, VkSampler sampler, VkImageLayout layout)
	{
		// P-F: appends to the versioned write log; the write lands on a
		// slot's set when that slot begins recording (ApplyDescriptorLog)
		// - which, by the timeline, is after the slot's previous submission
		// has retired, so a rewrite of a live index is always legal.
		DescriptorWriteLogEntry entry;
		entry.Kind_ = DescriptorWriteLogEntry::Kind::Texture;
		entry.Index = index;
		entry.ImageView = imageView;
		entry.Sampler = sampler;
		entry.ImageLayout = layout;

		std::lock_guard lock(m_DescriptorLogMutex);
		entry.Seq = m_NextDescriptorSeq++;
		m_DescriptorWriteLog.push_back(entry);
		RecordCurrentDescriptor(entry);
	}

	uint32_t VulkanRhiDevice::AllocateBindlessBufferIndex()
	{
		std::lock_guard lock(m_DescriptorLogMutex);

		uint32_t index;
		if (!m_FreeBindlessBufferIndices.empty())
		{
			index = m_FreeBindlessBufferIndices.back();
			m_FreeBindlessBufferIndices.pop_back();

			// A recycled index's old buffer entries are superseded by the
			// new owner's write (the texture binding's stay).
			DropSupersededEntries(m_DescriptorWriteLog, index, 1);
		}
		else
		{
			TE_CORE_ASSERT(m_NextBindlessBufferIndex < kBindlessBuffers, "bindless buffer array exhausted");
			index = m_NextBindlessBufferIndex++;
		}
		return index;
	}

	void VulkanRhiDevice::WriteBufferDescriptor(uint32_t index, VkBuffer buffer)
	{
		DescriptorWriteLogEntry entry;
		entry.Kind_ = DescriptorWriteLogEntry::Kind::Buffer;
		entry.Binding = 1;
		entry.Index = index;
		entry.Buffer = buffer;

		std::lock_guard lock(m_DescriptorLogMutex);
		entry.Seq = m_NextDescriptorSeq++;
		m_DescriptorWriteLog.push_back(entry);
		RecordCurrentDescriptor(entry);
	}

	void VulkanRhiDevice::RecordCurrentDescriptor(const DescriptorWriteLogEntry& entry)
	{
		std::vector<DescriptorWriteLogEntry>& table = entry.Binding == 1 ? m_CurrentBufferDescriptors : m_CurrentTextureDescriptors;
		if (entry.Index >= table.size())
			table.resize((size_t)entry.Index + 1);
		table[entry.Index] = entry;
	}

	void VulkanRhiDevice::ApplyDescriptorLog(uint32_t slot)
	{
		TE_CORE_ASSERT(slot < VulkanDevice::TotalFrameSlots, "descriptor apply with an out-of-range frame slot");

		std::lock_guard lock(m_DescriptorLogMutex);

		const uint64_t lastApplied = m_LastAppliedSeq[slot];
		// A slot's first apply (at birth, or after NoteSlotExpected)
		// re-seeds the WHOLE set from the current-descriptor tables - the
		// log's GC has already dropped every entry the other slots applied,
		// so the tail alone left a mid-session window with most of its set
		// unwritten.
		const bool reseed = !m_SlotSeeded[slot];

		// Collect the entries this slot hasn't applied yet (they are
		// already in log order, seq ascending - the apply lower_bounds
		// the monotonic seq instead of walking the whole log from 0).
		// The scratch buffers are reused across applies (the old form
		// allocated three log-sized vectors per slot per frame).
		std::vector<VkWriteDescriptorSet>& writes = m_ApplyScratchWrites;
		std::vector<VkDescriptorImageInfo>& imageInfos = m_ApplyScratchImageInfos;
		std::vector<VkDescriptorBufferInfo>& bufferInfos = m_ApplyScratchBufferInfos;
		writes.clear();
		imageInfos.clear();
		bufferInfos.clear();
		const auto pendingBegin = std::lower_bound(m_DescriptorWriteLog.begin(), m_DescriptorWriteLog.end(), lastApplied,
			[](const DescriptorWriteLogEntry& entry, uint64_t seq) { return entry.Seq <= seq; });
		std::vector<const DescriptorWriteLogEntry*>& pending = m_ApplyScratchEntries;
		pending.clear();
		if (reseed)
		{
			for (const DescriptorWriteLogEntry& entry : m_CurrentTextureDescriptors)
				if (entry.Seq != 0)
					pending.push_back(&entry);
			for (const DescriptorWriteLogEntry& entry : m_CurrentBufferDescriptors)
				if (entry.Seq != 0)
					pending.push_back(&entry);
		}
		else
		{
			for (auto it = pendingBegin; it != m_DescriptorWriteLog.end(); ++it)
				pending.push_back(&*it);
		}
		const size_t pendingCount = pending.size();
		writes.reserve(pendingCount);
		imageInfos.reserve(pendingCount);
		bufferInfos.reserve(pendingCount);

		uint64_t maxSeq = lastApplied;
		for (const DescriptorWriteLogEntry* pendingEntry : pending)
		{
			const DescriptorWriteLogEntry& entry = *pendingEntry;

			VkWriteDescriptorSet write{};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = m_GlobalSets[slot];
			write.dstBinding = entry.Binding;
			write.dstArrayElement = entry.Index;
			write.descriptorCount = 1;

			if (entry.Kind_ == DescriptorWriteLogEntry::Kind::Buffer)
			{
				// The binding-1 storage-buffer side. A tombstone entry for a
				// freed buffer slot carries the null buffer (a real
				// descriptor - nulling the slot needs the nullDescriptor
				// feature, which is not enabled).
				VkDescriptorBufferInfo bufferInfo{};
				bufferInfo.buffer = entry.Buffer;
				bufferInfo.offset = 0;
				bufferInfo.range = VK_WHOLE_SIZE;
				bufferInfos.push_back(bufferInfo);

				write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				write.pBufferInfo = &bufferInfos.back();
				writes.push_back(write);
				maxSeq = std::max(maxSeq, entry.Seq);
				continue;
			}

			VkDescriptorImageInfo imageInfo{};
			imageInfo.imageLayout = entry.ImageLayout;
			imageInfo.imageView = entry.ImageView;
			imageInfo.sampler = entry.Sampler; // a tombstone writes null handles
			imageInfos.push_back(imageInfo);

			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = &imageInfos.back();
			writes.push_back(write);
			maxSeq = std::max(maxSeq, entry.Seq);
		}
		// The re-seed covers every append so far (the tables hold the
		// latest entry of each index the log ever named).
		if (reseed)
		{
			maxSeq = std::max(maxSeq, m_NextDescriptorSeq - 1);
			m_SlotSeeded[slot] = true;
		}

		if (!writes.empty())
			vkUpdateDescriptorSets(m_Vulkan.GetDevice(), (uint32_t)writes.size(), writes.data(), 0, nullptr);
		m_LastAppliedSeq[slot] = maxSeq; // lastApplied when nothing was pending

		// GC: an entry every slot has applied is dead weight (the log would
		// otherwise grow forever across a long session's loads/unloads).
		const uint64_t floor = MinLastAppliedSeq();
		size_t keep = 0;
		for (size_t i = 0; i < m_DescriptorWriteLog.size(); i++)
		{
			if (m_DescriptorWriteLog[i].Seq <= floor)
				continue;
			if (keep != i)
				m_DescriptorWriteLog[keep] = m_DescriptorWriteLog[i];
			keep++;
		}
		m_DescriptorWriteLog.resize(keep);

		TryRecyclePendingIndices();
	}

	uint64_t VulkanRhiDevice::MinLastAppliedSeq() const
	{
		// The min over the EXPECTED slots. An expected slot that hasn't
		// applied yet reads as the seq 0 - the floor stays 0 until it
		// applies, so no index pending reuse can recycle before its
		// tombstone reaches that slot (the first apply is the whole-set
		// re-seed, which covers every append so far). The never-expected
		// slots (the never-opened windows) stay out.
		uint64_t min = UINT64_MAX;
		for (uint32_t i = 0; i < VulkanDevice::TotalFrameSlots; i++)
		{
			if (!m_SlotExpected[i])
				continue;
			min = std::min(min, m_LastAppliedSeq[i]);
		}
		return min == UINT64_MAX ? 0u : min;
	}

	void VulkanRhiDevice::NoteSlotExpected(uint32_t base)
	{
		std::lock_guard lock(m_DescriptorLogMutex);
		for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
		{
			const uint32_t slot = base + i;
			if (slot < VulkanDevice::TotalFrameSlots)
			{
				m_SlotExpected[slot] = true;
				// The slot's next apply re-seeds its whole set, and until then
				// it pins the GC floor at 0 (a recycled slot's old seq must
				// not count).
				m_SlotSeeded[slot] = false;
				m_LastAppliedSeq[slot] = 0;
			}
		}
	}

	void VulkanRhiDevice::DeactivateWindowSlots(uint32_t base)
	{
		std::lock_guard lock(m_DescriptorLogMutex);
		for (uint32_t i = 0; i < VulkanDevice::FramesInFlight; i++)
		{
			const uint32_t slot = base + i;
			if (slot < VulkanDevice::TotalFrameSlots)
				m_SlotExpected[slot] = false;
		}
	}

	void VulkanRhiDevice::TryRecyclePendingIndices()
	{
		// m_DescriptorLogMutex held. An index becomes reusable once every
		// slot's set has applied the tombstone that cleared it.
		const uint64_t floor = MinLastAppliedSeq();

		size_t write = 0;
		for (size_t i = 0; i < m_PendingIndexReuse.size(); i++)
		{
			if (m_PendingIndexReuse[i].second <= floor)
			{
				m_FreeBindlessIndices.push_back(m_PendingIndexReuse[i].first);
				continue;
			}
			if (write != i)
				m_PendingIndexReuse[write] = m_PendingIndexReuse[i];
			write++;
		}
		m_PendingIndexReuse.resize(write);

		// The buffer binding's own pending list.
		write = 0;
		for (size_t i = 0; i < m_PendingBufferIndexReuse.size(); i++)
		{
			if (m_PendingBufferIndexReuse[i].second <= floor)
			{
				m_FreeBindlessBufferIndices.push_back(m_PendingBufferIndexReuse[i].first);
				continue;
			}
			if (write != i)
				m_PendingBufferIndexReuse[write] = m_PendingBufferIndexReuse[i];
			write++;
		}
		m_PendingBufferIndexReuse.resize(write);
	}

	void VulkanRhiDevice::QueueTextureForDestruction(VkImage image, VmaAllocation allocation,
		VkImageView imageView, VkSampler sampler, uint32_t bindlessIndex)
	{
		RetireEntry entry;
		entry.Kind_ = RetireEntry::Kind::Texture;
		entry.Image = image;
		entry.Allocation = allocation;
		entry.ImageView = imageView;
		entry.Sampler = sampler;
		entry.BindlessIndex = bindlessIndex;
		// A barrier-only adoption carries the ~0u sentinel (no bindless
		// slot) - it must never enter the free list or get a tombstone.
		entry.HasBindlessIndex = bindlessIndex < kBindlessTextures;

		// P-F: the tombstone re-points the slot at the null texture (a REAL
		// descriptor - nulling the slot needs the nullDescriptor feature,
		// which even enabled faults the current NVIDIA driver). The index
		// is recycled only after every slot applied it.
		if (entry.HasBindlessIndex)
		{
			DescriptorWriteLogEntry tombstone;
			tombstone.Kind_ = DescriptorWriteLogEntry::Kind::Tombstone;
			tombstone.Index = bindlessIndex;
			tombstone.ImageView = m_NullTextureView;
			tombstone.Sampler = m_NullTextureSampler;
			tombstone.ImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			std::lock_guard lock(m_DescriptorLogMutex);
			tombstone.Seq = m_NextDescriptorSeq++;
			m_DescriptorWriteLog.push_back(tombstone);
			RecordCurrentDescriptor(tombstone);
			entry.TombstoneSeq = tombstone.Seq;
		}

		QueueRetire(std::move(entry));
	}

	void VulkanRhiDevice::QueueBufferForDestruction(VkBuffer buffer, VmaAllocation allocation, uint32_t bindlessIndex, BufferHandle slot)
	{
		RetireEntry entry;
		entry.Kind_ = RetireEntry::Kind::Buffer;
		entry.Buffer = buffer;
		entry.Allocation = allocation;
		entry.BufferSlot = slot;
		entry.BindlessIndex = bindlessIndex;
		entry.HasBindlessIndex = bindlessIndex < kBindlessBuffers;

		// The buffer slot's tombstone re-points the descriptor at the
		// 16-byte null buffer (a REAL descriptor); the index recycles only
		// after every slot applied it - the texture side's own contract.
		if (entry.HasBindlessIndex)
		{
			DescriptorWriteLogEntry tombstone;
			tombstone.Kind_ = DescriptorWriteLogEntry::Kind::Buffer;
			tombstone.Binding = 1;
			tombstone.Index = bindlessIndex;
			tombstone.Buffer = m_NullBuffer;
			std::lock_guard lock(m_DescriptorLogMutex);
			tombstone.Seq = m_NextDescriptorSeq++;
			m_DescriptorWriteLog.push_back(tombstone);
			RecordCurrentDescriptor(tombstone);
			entry.TombstoneSeq = tombstone.Seq;
		}

		QueueRetire(std::move(entry));
	}

	void VulkanRhiDevice::QueuePipelineForDestruction(VkPipeline pipeline, VkPipelineLayout layout)
	{
		RetireEntry entry;
		entry.Kind_ = RetireEntry::Kind::Pipeline;
		entry.Pipeline = pipeline;
		entry.Layout = layout;
		QueueRetire(std::move(entry));
	}

	void VulkanRhiDevice::QueueShaderForDestruction(VkShaderModule shader)
	{
		RetireEntry entry;
		entry.Kind_ = RetireEntry::Kind::Shader;
		entry.ShaderModule = shader;
		QueueRetire(std::move(entry));
	}

	void VulkanRhiDevice::QueueSamplerForDestruction(VkSampler sampler)
	{
		RetireEntry entry;
		entry.Kind_ = RetireEntry::Kind::Sampler;
		entry.Sampler = sampler;
		QueueRetire(std::move(entry));
	}

	void VulkanRhiDevice::QueueRetire(RetireEntry&& entry)
	{
		// The frame-boundary tagging - the entry frees once the SLOT's
		// record-start resets reach the tag (the frame's own command buffers
		// referencing the object are freed at the slot's reset TWO
		// record-starts later, and that reset's acquire-fence wait covers
		// the frame's submission completion). No timeline prediction: an
		// interleaved upload batch or another window's submit can no longer
		// consume the stamp (the old last-submitted+1 form freed one
		// submission early). The secondary windows' extracts bucket into the
		// MAIN window's slot (their resources are the swapchain's own, whose
		// teardown parks the device anyway) - the documented approximation.
		// The retire queue is game-thread-owned - asserted here, in both
		// drains and in the size accessor. A GPU object's last Ref dropped
		// on any other thread is a defect, not a case to bucket: a caller
		// census counted 0 non-game callers in 18400 retires (the captures,
		// the 600-frame and two-window runs, forced swapchain rebuilds, the
		// load/evict churn). The swapchain rebuild runs on the game thread
		// under the submit-idle park; asset workers only parse; recording
		// never creates or destroys device objects.
		// The slot tag is the game thread's current slot, read from the main
		// context. The one named exception is NO main context: before the
		// first window's Init, and after the main window's teardown cleared
		// it (the shutdown-time destroys). Those entries tag slot 0, and the
		// ShutdownDevice unconditional drain frees them.
		AssertRetireQueueOwner("QueueRetire");
		entry.Slot = 0;
		if (VulkanContext* context = m_Vulkan.TryGetMainContext())
			entry.Slot = context->GetCurrentFrameSlot();
		entry.ResetTag = m_SlotResetCount[entry.Slot].load(std::memory_order_relaxed) + kRetireResetDistance;
		m_RetireQueue.push_back(std::move(entry));
	}

	void VulkanRhiDevice::AssertRetireQueueOwner(const char* entryPoint) const
	{
		(void)entryPoint;
		TE_CORE_ASSERT(m_Vulkan.IsGameThread(), std::string("VulkanRhiDevice::") + entryPoint + " off the game thread - the retire queue is game-thread-owned; a GPU object's last Ref must drop on the game thread");
	}

	size_t VulkanRhiDevice::GetRetireQueueSize() const
	{
		AssertRetireQueueOwner("GetRetireQueueSize");
		return m_RetireQueue.size();
	}

	void VulkanRhiDevice::NoteSlotReset(uint32_t slot)
	{
		if (slot < VulkanDevice::TotalFrameSlots)
			m_SlotResetCount[slot].fetch_add(1u, std::memory_order_release);
	}

	void VulkanRhiDevice::FreeRetireEntry(const RetireEntry& entry)
	{
		// A destroyed RHI buffer's Retiring table slot releases with its
		// VkBuffer - the handle resolved until now (the record of a frame
		// that named it is done by the slot's reset tag). The slot releases
		// BEFORE the VkBuffer is destroyed (the entry carries its own
		// Buffer/Allocation copies): a future reset-tag violation then
		// resolves to VK_NULL_HANDLE, which validation reports, never to a
		// destroyed VkBuffer. The table's own lock, outside the descriptor
		// log lock, which the release never needs.
		if (entry.Kind_ == RetireEntry::Kind::Buffer && IsValid(entry.BufferSlot))
		{
			const bool released = m_Buffers.Release(entry.BufferSlot);
			TE_CORE_ASSERT(released, "retiring an invalid/stale RHI buffer slot");
			(void)released;
		}

		FreeRetireEntryObjects(entry);
	}

	bool VulkanRhiDevice::IsBufferSlotRecycled(BufferHandle handle) const
	{
		std::shared_lock lock(m_Buffers.Mutex);
		if (handle.Index >= m_Buffers.Slots.size())
			return false;
		const auto& slot = m_Buffers.Slots[handle.Index];
		if (slot.Alive || slot.Generation == handle.Generation)
			return false;
		return std::find(m_Buffers.Free.begin(), m_Buffers.Free.end(), handle.Index) != m_Buffers.Free.end();
	}

	void VulkanRhiDevice::FreeRetireEntryObjects(const RetireEntry& entry)
	{
		// P-F: the destruction of a texture's Vk view and the purge of its
		// REAL log entries must be atomic w.r.t. ApplyDescriptorLog - a
		// record worker mid-apply holds this mutex, so destroying the view
		// only here (under the same lock) guarantees no apply can ever
		// touch a freed VkImageView, and no worker that already copied the
		// write can race the free (DrainRetireQueue runs on the game
		// thread, the apply on record workers).
		std::lock_guard lock(m_DescriptorLogMutex);

		switch (entry.Kind_)
		{
		case RetireEntry::Kind::Texture:
			if (entry.Sampler != VK_NULL_HANDLE)
				vkDestroySampler(m_Vulkan.GetDevice(), entry.Sampler, nullptr);
			if (entry.ImageView != VK_NULL_HANDLE)
				vkDestroyImageView(m_Vulkan.GetDevice(), entry.ImageView, nullptr);
			if (entry.Image != VK_NULL_HANDLE)
				vmaDestroyImage(m_Vulkan.GetAllocator(), entry.Image, entry.Allocation);
			break;
		case RetireEntry::Kind::Buffer:
			if (entry.Buffer != VK_NULL_HANDLE)
				vmaDestroyBuffer(m_Vulkan.GetAllocator(), entry.Buffer, entry.Allocation);
			break;
		case RetireEntry::Kind::Pipeline:
			if (entry.Pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(m_Vulkan.GetDevice(), entry.Pipeline, nullptr);
			if (entry.Layout != VK_NULL_HANDLE)
				vkDestroyPipelineLayout(m_Vulkan.GetDevice(), entry.Layout, nullptr);
			break;
		case RetireEntry::Kind::Shader:
			if (entry.ShaderModule != VK_NULL_HANDLE)
				vkDestroyShaderModule(m_Vulkan.GetDevice(), entry.ShaderModule, nullptr);
			break;
		case RetireEntry::Kind::Sampler:
			if (entry.Sampler != VK_NULL_HANDLE)
				vkDestroySampler(m_Vulkan.GetDevice(), entry.Sampler, nullptr);
			break;
		}
		if (entry.HasBindlessIndex)
		{
			// P-F: the index returns to the free list only when its
			// tombstone has been applied to EVERY slot's set (until then
			// some slot's set still names the destroyed view).
			// Every REAL write for this index is now dead weight that must
			// never be applied (a slot replaying it would dereference the
			// freed view, and the min-lastApplied GC floor never advances
			// while any slot is idle). Drop them; the tombstone stays to
			// clear the slots.
			const uint32_t binding = entry.Kind_ == RetireEntry::Kind::Buffer ? 1u : 0u;
			for (auto it = m_DescriptorWriteLog.begin(); it != m_DescriptorWriteLog.end();)
			{
				if (it->Index == entry.BindlessIndex && it->Binding == binding && it->Kind_ != DescriptorWriteLogEntry::Kind::Tombstone)
					it = m_DescriptorWriteLog.erase(it);
				else
					++it;
			}
			// The re-seed table must never name the freed view/buffer either
			// - a write that followed the tombstone (dropped from the log
			// above) falls back to the null descriptor the tombstone wrote.
			std::vector<DescriptorWriteLogEntry>& table = binding == 1 ? m_CurrentBufferDescriptors : m_CurrentTextureDescriptors;
			if (entry.TombstoneSeq != 0 && entry.BindlessIndex < table.size() && table[entry.BindlessIndex].Seq > entry.TombstoneSeq)
			{
				DescriptorWriteLogEntry& current = table[entry.BindlessIndex];
				if (binding == 1)
				{
					current.Buffer = m_NullBuffer;
				}
				else
				{
					current.Kind_ = DescriptorWriteLogEntry::Kind::Tombstone;
					current.ImageView = m_NullTextureView;
					current.Sampler = m_NullTextureSampler;
					current.ImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				}
			}

			const bool recycled = entry.TombstoneSeq == 0 || entry.TombstoneSeq <= MinLastAppliedSeq();
			if (entry.Kind_ == RetireEntry::Kind::Buffer)
			{
				if (recycled)
					m_FreeBindlessBufferIndices.push_back(entry.BindlessIndex);
				else
					m_PendingBufferIndexReuse.emplace_back(entry.BindlessIndex, entry.TombstoneSeq);
			}
			else
			{
				if (recycled)
					m_FreeBindlessIndices.push_back(entry.BindlessIndex);
				else
					m_PendingIndexReuse.emplace_back(entry.BindlessIndex, entry.TombstoneSeq);
			}
		}
	}

	void VulkanRhiDevice::DrainRetireQueue()
	{
		AssertRetireQueueOwner("DrainRetireQueue");
		if (m_RetireQueue.empty())
			return;

		// The per-entry reset-tag check (the slot's record-start
		// resets) instead of the completed-timeline stamp.
		size_t write = 0;
		for (size_t i = 0; i < m_RetireQueue.size(); i++)
		{
			const RetireEntry& entry = m_RetireQueue[i];
			if (entry.ResetTag <= m_SlotResetCount[entry.Slot].load(std::memory_order_acquire))
			{
				FreeRetireEntry(entry);
				continue;
			}
			if (write != i)
				m_RetireQueue[write] = std::move(m_RetireQueue[i]);
			write++;
		}
		m_RetireQueue.resize(write);
	}

	void VulkanRhiDevice::DrainRetireQueueUnconditional()
	{
		AssertRetireQueueOwner("DrainRetireQueueUnconditional");
		for (const RetireEntry& entry : m_RetireQueue)
			FreeRetireEntry(entry);
		m_RetireQueue.clear();
	}

	bool VulkanRhiDevice::IsBindlessIndexLive(uint32_t index) const
	{
		std::lock_guard lock(m_DescriptorLogMutex);
		if (index >= m_NextBindlessIndex)
			return false;
		for (uint32_t freeIndex : m_FreeBindlessIndices)
			if (freeIndex == index)
				return false;
		for (const auto& [pendingIndex, seq] : m_PendingIndexReuse)
			if (pendingIndex == index)
				return false;
		return true;
	}

	bool VulkanRhiDevice::IsBindlessIndexRecycled(uint32_t index) const
	{
		std::lock_guard lock(m_DescriptorLogMutex);
		for (uint32_t freeIndex : m_FreeBindlessIndices)
			if (freeIndex == index)
				return true;
		return false;
	}

	// ---- Buffer -----------------------------------------------------------------

	BufferHandle VulkanRhiDevice::CreateBuffer(const BufferDesc& desc, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI buffer created before the device");
		TE_CORE_ASSERT(desc.Usage != BufferUsage::None, "RHI buffer created with no usage (BufferDesc::Usage is explicit)");
		VkBufferUsageFlags usage = 0;
		if (HasUsage(desc.Usage, BufferUsage::Vertex))
			usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if (HasUsage(desc.Usage, BufferUsage::Index))
			usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (HasUsage(desc.Usage, BufferUsage::Storage))
			usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		// A device-local buffer is filled only through UploadBufferViaStaging
		// (the static upload), so it is always a transfer destination.
		if (HasUsage(desc.Usage, BufferUsage::TransferDst) || !desc.HostVisible)
			usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

		BufferHandle handle = m_Buffers.Acquire();
		VulkanBuffer* buffer = new VulkanBuffer();
		buffer->Init(m_Vulkan, desc.Size, usage, desc.HostVisible, debugName);
		m_Buffers.Publish(handle, std::unique_ptr<VulkanBuffer>(buffer));
		return handle;
	}

	void VulkanRhiDevice::DestroyBuffer(BufferHandle handle)
	{
		// The slot is marked Retiring (dead to the owner) and released by
		// FreeRetireEntry with the VkBuffer, so the submit thread's
		// in-flight record of the previous frame can still resolve the
		// handle. BeginRetire is a real side effect, kept out of the assert
		// so it runs in every config (a slot left alive is double-queued by
		// the ShutdownDevice sweep).
		VulkanBuffer* buffer = m_Buffers.BeginRetire(handle);
		TE_CORE_ASSERT(buffer, "destroying an invalid/stale RHI buffer");
		if (buffer)
			QueueBufferForDestruction(buffer->GetHandle(), buffer->GetAllocation(), buffer->GetBindlessIndex(), handle);
	}

	void VulkanRhiDevice::WriteBindlessBuffer(uint32_t index, BufferHandle handle)
	{
		VulkanBuffer* buffer = m_Buffers.Get(handle);
		TE_CORE_ASSERT(buffer, "WriteBindlessBuffer with an invalid/stale buffer handle");
		// The slot is remembered so DestroyBuffer's retire path tombstones
		// the descriptor + recycles the index.
		buffer->SetBindlessIndex(index);
		WriteBufferDescriptor(index, buffer->GetHandle());
	}

	void VulkanRhiDevice::SetBufferData(BufferHandle handle, const void* data, uint64_t size, uint64_t offset)
	{
		if (VulkanBuffer* buffer = m_Buffers.Get(handle))
			buffer->SetData(data, size, offset);
	}

	const void* VulkanRhiDevice::GetBufferMappedData(BufferHandle handle)
	{
		VulkanBuffer* buffer = m_Buffers.Get(handle);
		return buffer ? buffer->GetMappedData() : nullptr;
	}

	void VulkanRhiDevice::UploadBufferViaStaging(BufferHandle handle, const void* data, uint64_t size)
	{
		if (VulkanBuffer* buffer = m_Buffers.Get(handle))
		{
			TE_CORE_ASSERT((buffer->GetUsage() & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0, "UploadBufferViaStaging into a buffer created without transfer-dst usage");
			buffer->UploadViaStaging(data, size);
		}
	}

	// ---- Texture ------------------------------------------------------------------

	TextureHandle VulkanRhiDevice::CreateTexture(const TextureDesc& desc, const void* initialData, uint64_t dataSize, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI texture created before the device");

		TextureHandle handle = m_Textures.Acquire();
		VulkanTexture2D* texture = nullptr;
		// The graph-transient RENDER TARGET path - an attachment in the
		// requested format, sampled back by the lighting/post passes. Depth
		// transients are NOT needed (the G-buffer pass shares the target's
		// own D32 depth) - assert instead of half-supporting.
		if (desc.RenderTarget || desc.DepthTarget)
		{
			TE_CORE_ASSERT(!desc.DepthTarget, "depth render-target transients unsupported - the G-buffer shares the target's D32");
			TE_CORE_ASSERT(desc.MipLevels == 1, "render-target transients are single-level");
			texture = new VulkanTexture2D(desc.Width, desc.Height, ToVkFormat(desc.PixelFormat), false);
		}
		else if (desc.MipLevels == 1)
		{
			// P-D: a genuinely single-level texture (the baked-mip ctor
			// uploads each level directly - no blit chain). The fixed
			// sampler's maxLod-0 clamp + one level is what reproduces
			// Wisp's single-level filtering exactly.
			std::vector<std::vector<uint8_t>> levels;
			if (initialData && dataSize > 0)
				levels.push_back(std::vector<uint8_t>((const uint8_t*)initialData, (const uint8_t*)initialData + dataSize));
			else
				levels.push_back(std::vector<uint8_t>((size_t)desc.Width * desc.Height * 4, 0));
			texture = new VulkanTexture2D(desc.Width, desc.Height, levels);
		}
		else
		{
			texture = new VulkanTexture2D(desc.Width, desc.Height);
			if (initialData && dataSize > 0)
				texture->SetData(const_cast<void*>(initialData), (uint32_t)dataSize);
		}
		std::unique_ptr<VulkanTextureEntry> entry = std::make_unique<VulkanTextureEntry>();
		entry->Owned.reset(texture);
		m_Textures.Publish(handle, std::move(entry));

		// The texture's ctor already wrote its bindless descriptor with its
		// baked (linear, repeat) sampler; for the single-level path, re-write
		// with the fixed sampler the caller chose (UPDATE_AFTER_BIND - safe
		// at creation). Multi-mip textures keep the baked sampler (a fixed
		// maxLod-0 sampler would defeat the mip chain).
		if (desc.MipLevels == 1)
		{
			WriteTextureDescriptor(texture->GetBindlessIndex(), texture->GetImageView(),
				desc.Sampler == SamplerKind::Nearest ? m_FixedSamplerNearest : m_FixedSamplerLinear,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		(void)debugName;
		return handle;
	}

	TextureHandle VulkanRhiDevice::CreateTextureFromBakedMips(uint32_t width, uint32_t height,
		const std::vector<std::vector<uint8_t>>& mipLevels, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI texture created before the device");
		TextureHandle handle = m_Textures.Acquire();
		std::unique_ptr<VulkanTextureEntry> entry = std::make_unique<VulkanTextureEntry>();
		entry->Owned.reset(new VulkanTexture2D(width, height, mipLevels));
		m_Textures.Publish(handle, std::move(entry));
		(void)debugName;
		return handle;
	}

	TextureHandle VulkanRhiDevice::CreateTextureFromNativeImage(void* nativeImage, uint32_t width, uint32_t height, Format format)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI wrapped image created before the device");
		TE_CORE_ASSERT(nativeImage, "wrapping a null image");
		TE_CORE_ASSERT(format == Format::B8G8R8A8_UNORM || format == Format::R8G8B8A8_UNORM,
			"wrapped images are BGRA8/RGBA8 only (the framebuffer/texture formats in use)");

		VkImage image = (VkImage)nativeImage;

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = ToVkFormat(format);
		viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageView view = VK_NULL_HANDLE;
		VulkanCheckResult(vkCreateImageView(m_Vulkan.GetDevice(), &viewInfo, nullptr, &view), "vkCreateImageView", __FILE__, __LINE__);
		m_Vulkan.SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, (uint64_t)view, "Wrapped External Image View");

		uint32_t index = AllocateBindlessTextureIndex();
		WriteTextureDescriptor(index, view, m_FixedSamplerLinear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		TextureHandle handle = m_Textures.Acquire();
		std::unique_ptr<VulkanTextureEntry> entry = std::make_unique<VulkanTextureEntry>();
		entry->Wrapped.reset(new VulkanWrappedTexture(index, view, image));
		m_Textures.Publish(handle, std::move(entry));
		(void)width; (void)height;
		return handle;
	}

	TextureHandle VulkanRhiDevice::AdoptExternalImage(void* nativeImage)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI image adopted before the device");
		TE_CORE_ASSERT(nativeImage, "adopting a null image");

		// Barrier-target adoption: no view, no bindless slot (the sentinel
		// index never recycles). Destroy releases the handle only.
		TextureHandle handle = m_Textures.Acquire();
		std::unique_ptr<VulkanTextureEntry> entry = std::make_unique<VulkanTextureEntry>();
		entry->Wrapped.reset(new VulkanWrappedTexture(~0u, VK_NULL_HANDLE, (VkImage)nativeImage));
		m_Textures.Publish(handle, std::move(entry));
		return handle;
	}

	void VulkanRhiDevice::DestroyTexture(TextureHandle handle)
	{
		// The wrapped-image path retires the view + slot only (the image is
		// caller-owned; the fixed sampler must never enter a retire entry).
		// Barrier-only adoptions have no view - nothing to retire. The owned
		// path's Release invokes the queuing VulkanTexture2D dtor.
		if (VulkanTextureEntry* entry = m_Textures.Get(handle))
		{
			if (entry->IsWrapped() && entry->Wrapped->GetView() != VK_NULL_HANDLE)
				QueueTextureForDestruction(VK_NULL_HANDLE, VK_NULL_HANDLE, entry->Wrapped->GetView(), VK_NULL_HANDLE, entry->Wrapped->GetBindlessIndex());
		}
		const bool textureReleased = m_Textures.Release(handle);
		TE_CORE_ASSERT(textureReleased, "destroying an invalid/stale RHI texture");
	}

	void VulkanRhiDevice::SetTextureData(TextureHandle handle, const void* data, uint64_t size)
	{
		if (VulkanTextureEntry* entry = m_Textures.Get(handle))
		{
			if (entry->Owned)
				entry->Owned->SetData(const_cast<void*>(data), (uint32_t)size);
		}
	}

	void VulkanRhiDevice::UpdateTextureBakedMips(TextureHandle handle, const std::vector<std::vector<uint8_t>>& mipLevels)
	{
		if (VulkanTextureEntry* entry = m_Textures.Get(handle))
		{
			if (entry->Owned)
				entry->Owned->UpdateBakedMips(entry->Owned->GetWidth(), entry->Owned->GetHeight(), mipLevels);
		}
	}

	uint32_t VulkanRhiDevice::GetBindlessIndex(TextureHandle handle)
	{
		if (const VulkanTextureEntry* entry = m_Textures.Get(handle))
		{
			// Barrier-only adoptions carry the ~0u sentinel (no bindless
			// slot) - never hand it to a caller.
			uint32_t index = entry->GetBindlessIndex();
			return index == ~0u ? 0 : index;
		}
		return 0;
	}

	// ---- Sampler / Shader / Pipeline ----------------------------------------------

	SamplerHandle VulkanRhiDevice::CreateSampler(const SamplerDesc& desc, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI sampler created before the device");
		SamplerHandle handle = m_Samplers.Acquire();
		VulkanSampler* sampler = new VulkanSampler();
		sampler->Init(m_Vulkan, desc, debugName);
		m_Samplers.Publish(handle, std::unique_ptr<VulkanSampler>(sampler));
		return handle;
	}

	void VulkanRhiDevice::DestroySampler(SamplerHandle handle)
	{
		if (VulkanSampler* sampler = m_Samplers.Get(handle))
			QueueSamplerForDestruction(sampler->GetHandle());
		const bool samplerReleased = m_Samplers.Release(handle);
		TE_CORE_ASSERT(samplerReleased, "destroying an invalid/stale RHI sampler");
	}

	ShaderHandle VulkanRhiDevice::CreateShader(std::span<const uint32_t> spirv, ShaderStage stage, const char* debugName, const ShaderIdentity* identity)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI shader created before the device");
		ShaderHandle handle = m_Shaders.Acquire();
		VulkanShaderModule* shader = new VulkanShaderModule();
		shader->Init(m_Vulkan, std::vector<uint32_t>(spirv.begin(), spirv.end()), ToVkStage(stage), debugName);
		if (identity)
		{
			shader->Identity = *identity;
			shader->HasIdentity = true;
		}
		m_Shaders.Publish(handle, std::unique_ptr<VulkanShaderModule>(shader));
		return handle;
	}

	Device::ShaderReloadResult VulkanRhiDevice::ReloadShader(std::string_view name, std::span<const uint32_t> spirv)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI shader reloaded before the device");
		ShaderReloadResult result;
		const std::vector<uint32_t> code(spirv.begin(), spirv.end());

		std::vector<VulkanShaderModule*> changed;
		{
			std::shared_lock lock(m_Shaders.Mutex);
			for (auto& slot : m_Shaders.Slots)
			{
				if (!slot.Alive || !slot.Object || slot.Object->GetName() != name)
					continue;
				result.Matched++;
				if (slot.Object->GetSpirV() != code) // the daemon re-broadcasts every asset per build
					changed.push_back(slot.Object.get());
			}
		}
		if (changed.empty())
			return result;

		// The CPU side builds the push block and binds the bindless array:
		// a shader that changed either needs that code changed too.
		const VulkanShaderReflectionInfo fresh = VulkanShaderReflection::Reflect(code);
		for (VulkanShaderModule* shader : changed)
		{
			const VulkanShaderReflectionInfo old = VulkanShaderReflection::Reflect(shader->GetSpirV());
			if (old.PushConstantSize != fresh.PushConstantSize)
			{
				result.Refusal = "its push-constant block changed (" + std::to_string(old.PushConstantSize) + " -> " +
				                 std::to_string(fresh.PushConstantSize) + " bytes)";
				return result;
			}
			if (old.HasCombinedImageSampler != fresh.HasCombinedImageSampler ||
			    (old.HasCombinedImageSampler && old.CombinedImageSamplerBinding != fresh.CombinedImageSamplerBinding))
			{
				result.Refusal = "its sampler binding changed";
				return result;
			}
		}

		// The new modules, then every cached pipeline baking a changed shader
		// rebuilt from its spec with them - nothing is swapped until all exist.
		std::vector<std::unique_ptr<VulkanShaderModule>> modules;
		std::unordered_map<const VulkanShaderModule*, VulkanShaderModule*> fresher;
		for (VulkanShaderModule* shader : changed)
		{
			auto module = std::make_unique<VulkanShaderModule>();
			module->Init(m_Vulkan, code, shader->GetStage(), std::string(name));
			fresher[shader] = module.get();
			modules.push_back(std::move(module));
		}
		struct Rebuilt
		{
			PipelineHandle Handle;
			VulkanPipeline Pipeline;
		};
		std::vector<Rebuilt> rebuilt;
		for (const auto& [key, entry] : m_PipelineCache)
		{
			auto vs = fresher.find(entry.Spec.VertexShader);
			auto fs = fresher.find(entry.Spec.FragmentShader);
			if (vs == fresher.end() && fs == fresher.end())
				continue;
			VulkanPipelineSpec spec = entry.Spec;
			if (vs != fresher.end())
				spec.VertexShader = vs->second;
			if (fs != fresher.end())
				spec.FragmentShader = fs->second;
			Rebuilt r;
			r.Handle = entry.Handle;
			r.Pipeline.Init(m_Vulkan, spec, entry.Name, m_VkPipelineCache);
			rebuilt.push_back(std::move(r));
		}

		// The swap: each slot takes its new object, the old ones retire (the
		// in-flight frames may still reference them). The cache entries' specs
		// keep pointing at the slot objects, which now hold the new modules.
		for (size_t i = 0; i < changed.size(); i++)
		{
			changed[i]->SwapContents(*modules[i]);
			QueueShaderForDestruction(modules[i]->GetHandle());
		}
		for (Rebuilt& r : rebuilt)
		{
			VulkanPipeline* pipeline = m_Pipelines.Get(r.Handle);
			TE_CORE_ASSERT(pipeline, "a cached pipeline without its slot");
			pipeline->SwapContents(r.Pipeline);
			QueuePipelineForDestruction(r.Pipeline.GetHandle(), r.Pipeline.GetLayout());
		}
		result.Shaders = (uint32_t)changed.size();
		result.Pipelines = (uint32_t)rebuilt.size();
		return result;
	}

	void VulkanRhiDevice::DestroyShader(ShaderHandle handle)
	{
		// Pipelines bake the shader modules in - evict every cached
		// pipeline referencing the dying shader first.
		std::vector<PipelineCacheKey> evict;
		for (const auto& [key, entry] : m_PipelineCache)
			if ((key.VertexShader.Index == handle.Index && key.VertexShader.Generation == handle.Generation) ||
				(key.FragmentShader.Index == handle.Index && key.FragmentShader.Generation == handle.Generation))
				evict.push_back(key);
		for (const PipelineCacheKey& key : evict)
			EvictPipelineEntry(key);

		if (VulkanShaderModule* shader = m_Shaders.Get(handle))
			QueueShaderForDestruction(shader->GetHandle());
		const bool shaderReleased = m_Shaders.Release(handle);
		TE_CORE_ASSERT(shaderReleased, "destroying an invalid/stale RHI shader");
	}

	uint64_t VulkanRhiDevice::HashPipelineStateDesc(const PipelineStateDesc& desc)
	{
		blake3_hasher hasher;
		blake3_hasher_init(&hasher);
		auto feed = [&](const void* p, size_t n) { blake3_hasher_update(&hasher, p, n); };
		uint64_t vs = ((uint64_t)desc.VertexShader.Index << 32) | desc.VertexShader.Generation;
		uint64_t fs = ((uint64_t)desc.FragmentShader.Index << 32) | desc.FragmentShader.Generation;
		feed(&vs, sizeof(vs));
		feed(&fs, sizeof(fs));
		feed(&desc.VertexStride, sizeof(desc.VertexStride));
		for (const auto& attribute : desc.VertexAttributes)
		{
			uint32_t location = attribute.Location;
			uint32_t format = (uint32_t)attribute.PixelFormat;
			uint32_t offset = attribute.Offset;
			feed(&location, sizeof(location));
			feed(&format, sizeof(format));
			feed(&offset, sizeof(offset));
		}
		uint32_t blend = desc.EnableBlend ? 1u : 0u;
		uint32_t blendSrc = (uint32_t)desc.ColorBlendSrc;
		uint32_t blendDst = (uint32_t)desc.ColorBlendDst;
		uint32_t preserveAlpha = desc.PreserveDestinationAlpha ? 1u : 0u;
		uint32_t depthTest = desc.DepthTestEnable ? 1u : 0u;
		uint32_t depthWrite = desc.DepthWriteEnable ? 1u : 0u;
		uint32_t compareOp = (uint32_t)desc.DepthCompareOp;
		uint32_t polygonMode = (uint32_t)desc.PolygonMode;
		uint32_t colourCount = desc.ColorAttachmentCount;
		uint32_t depthFormat = (uint32_t)desc.DepthAttachmentFormat;
		feed(&blend, sizeof(blend));
		feed(&blendSrc, sizeof(blendSrc));
		feed(&blendDst, sizeof(blendDst));
		feed(&preserveAlpha, sizeof(preserveAlpha));
		feed(&depthTest, sizeof(depthTest));
		feed(&depthWrite, sizeof(depthWrite));
		feed(&compareOp, sizeof(compareOp));
		feed(&polygonMode, sizeof(polygonMode));
		feed(&colourCount, sizeof(colourCount));
		for (uint32_t i = 0; i < desc.ColorAttachmentCount; ++i)
		{
			uint32_t colourFormat = (uint32_t)desc.ColorAttachmentFormats[i];
			feed(&colourFormat, sizeof(colourFormat));
		}
		feed(&depthFormat, sizeof(depthFormat));
		feed(&desc.PushConstantSize, sizeof(desc.PushConstantSize));
		feed(&desc.PushConstantStages, sizeof(desc.PushConstantStages));
		uint8_t out[BLAKE3_OUT_LEN];
		blake3_hasher_finalize(&hasher, out, sizeof(out));
		uint64_t hash = 0;
		for (int i = 0; i < 8; i++)
			hash = (hash << 8) | out[i];
		return hash;
	}

	PipelineHandle VulkanRhiDevice::CreatePipeline(const PipelineStateDesc& desc, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI pipeline created before the device");

		// The cache lookup - keyed by (state hash, vertex shader handle,
		// fragment shader handle). A hit returns the cached pipeline
		// (refcounted); a miss creates one through the driver's
		// VkPipelineCache.
		PipelineCacheKey key;
		key.StateHash = HashPipelineStateDesc(desc);
		key.VertexShader = desc.VertexShader;
		key.FragmentShader = desc.FragmentShader;
		auto cached = m_PipelineCache.find(key);
		if (cached != m_PipelineCache.end())
		{
			cached->second.RefCount++;
			return cached->second.Handle;
		}

		VulkanPipelineSpec spec;
		spec.VertexShader = m_Shaders.Get(desc.VertexShader);
		spec.FragmentShader = m_Shaders.Get(desc.FragmentShader);
		spec.VertexStride = desc.VertexStride;
		for (const auto& attribute : desc.VertexAttributes)
			spec.VertexAttributes.push_back({ attribute.Location, ToVkFormat(attribute.PixelFormat), attribute.Offset });
		spec.EnableBlend = desc.EnableBlend;
		spec.ColorBlendSrc = ToVkBlendFactor(desc.ColorBlendSrc);
		spec.ColorBlendDst = ToVkBlendFactor(desc.ColorBlendDst);
		spec.PreserveDestinationAlpha = desc.PreserveDestinationAlpha;
		spec.DepthTestEnable = desc.DepthTestEnable;
		spec.DepthWriteEnable = desc.DepthWriteEnable;
		spec.DepthCompareOp = ToVkCompareOp(desc.DepthCompareOp);
		spec.DepthBiasEnable = desc.DepthBiasEnable;
		spec.DepthBiasConstantFactor = desc.DepthBiasConstantFactor;
		spec.DepthBiasSlopeFactor = desc.DepthBiasSlopeFactor;
		spec.PolygonMode = ToVkPolygonMode(desc.PolygonMode);
		TE_CORE_ASSERT(desc.ColorAttachmentCount <= PipelineStateDesc::kMaxColorAttachments,
			"pipeline colour-attachment count over the RHI maximum");
		spec.ColorAttachmentCount = desc.ColorAttachmentCount;
		for (uint32_t i = 0; i < desc.ColorAttachmentCount; ++i)
			spec.ColorAttachmentFormats[i] = ToVkFormat(desc.ColorAttachmentFormats[i]);
		spec.DepthAttachmentFormat = ToVkFormat(desc.DepthAttachmentFormat);
		spec.PushConstantSize = desc.PushConstantSize;
		spec.PushConstantStages = desc.PushConstantStages;
		// P-A3: every RHI pipeline shares the global bindless set (set 0);
		// the layout is baked in implicitly - no PipelineStateDesc field.
		spec.DescriptorSetLayout = m_GlobalSetLayout;

		PipelineHandle handle = m_Pipelines.Acquire();
		VulkanPipeline* pipeline = new VulkanPipeline();
		pipeline->Init(m_Vulkan, spec, debugName, m_VkPipelineCache);
		m_Pipelines.Publish(handle, std::unique_ptr<VulkanPipeline>(pipeline));

		PipelineCacheEntry entry;
		entry.Handle = handle;
		entry.RefCount = 1;
		entry.Spec = spec;
		entry.Name = debugName ? debugName : "";
		m_PipelineCache.emplace(key, std::move(entry));
		return handle;
	}

	void VulkanRhiDevice::EvictPipelineEntry(const PipelineCacheKey& key)
	{
		auto it = m_PipelineCache.find(key);
		if (it == m_PipelineCache.end())
			return;
		const PipelineHandle handle = it->second.Handle;
		if (VulkanPipeline* pipeline = m_Pipelines.Get(handle))
			QueuePipelineForDestruction(pipeline->GetHandle(), pipeline->GetLayout());
		m_Pipelines.Release(handle);
		m_PipelineCache.erase(it);
	}

	void VulkanRhiDevice::DestroyPipeline(PipelineHandle handle)
	{
		// The refcounted cache path: the final destroy evicts the entry
		// (queueing the Vk objects + releasing the slot).
		for (auto it = m_PipelineCache.begin(); it != m_PipelineCache.end(); ++it)
		{
			if (it->second.Handle.Index == handle.Index && it->second.Handle.Generation == handle.Generation)
			{
				if (it->second.RefCount > 1)
				{
					it->second.RefCount--;
					return;
				}
				EvictPipelineEntry(it->first);
				return;
			}
		}

		if (VulkanPipeline* pipeline = m_Pipelines.Get(handle))
			QueuePipelineForDestruction(pipeline->GetHandle(), pipeline->GetLayout());
		const bool pipelineReleased = m_Pipelines.Release(handle);
		TE_CORE_ASSERT(pipelineReleased, "destroying an invalid/stale RHI pipeline");
	}

	void VulkanRhiDevice::InvalidatePipelines(const AssetID& shaderAsset)
	{
		std::vector<PipelineCacheKey> evict;
		for (const auto& [key, entry] : m_PipelineCache)
		{
			auto matchesAsset = [&](ShaderHandle handle) -> bool {
				VulkanShaderModule* shader = m_Shaders.Get(handle);
				return shader && shader->HasIdentity && shader->Identity.Asset == shaderAsset;
			};
			if (matchesAsset(key.VertexShader) || matchesAsset(key.FragmentShader))
				evict.push_back(key);
		}
		for (const PipelineCacheKey& key : evict)
			EvictPipelineEntry(key);
	}

	// ---- Command lists / queues / submit ------------------------------------------

	CommandListHandle VulkanRhiDevice::CreateCommandList(QueueHandle queue, uint32_t threadIndex)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI command list created before the device");
		TE_CORE_ASSERT(threadIndex == 0, "P-A1 records on the main thread only (thread pools land in P-A2)");
		(void)queue;

		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_Vulkan.GetCommandPool(0);
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		VulkanCheckResult(vkAllocateCommandBuffers(m_Vulkan.GetDevice(), &allocInfo, &commandBuffer),
			"vkAllocateCommandBuffers", __FILE__, __LINE__);

		CommandListHandle handle = m_CommandLists.Acquire();
		// The buffer comes from frame slot 0's thread-0 pool, so the list
		// records for slot 0 (no caller creates lists this way today; the
		// frame records through the per-job lists).
		m_CommandLists.Publish(handle, std::unique_ptr<VulkanCommandList>(new VulkanCommandList(this, commandBuffer, 0)));
		return handle;
	}

	void VulkanRhiDevice::DestroyCommandList(CommandListHandle handle)
	{
		const bool commandListReleased = m_CommandLists.Release(handle);
		TE_CORE_ASSERT(commandListReleased, "destroying an invalid/stale RHI command list");
	}

	CommandList* VulkanRhiDevice::GetCommandList(CommandListHandle handle)
	{
		return m_CommandLists.Get(handle);
	}

	QueueHandle VulkanRhiDevice::GetQueue(QueueKind kind)
	{
		// v1: all three kinds resolve to the single graphics queue.
		(void)kind;
		return m_GraphicsQueue;
	}

	uint64_t VulkanRhiDevice::Submit(QueueHandle queue, std::span<const CommandListHandle> commandLists,
		std::span<const TimelineWait> waits, std::span<const TimelineSignal> signals)
	{
		// P-A2: a real timeline submit. v1: all queues resolve to the one
		// graphics queue; its timeline is the only semaphore waits/signals
		// name, so multiple entries collapse to the single largest value.
		// Every submit signals one fresh value (allocated here when the
		// caller passes none) and returns it.
		(void)queue;

		std::vector<VkCommandBuffer> buffers;
		buffers.reserve(commandLists.size());
		for (CommandListHandle handle : commandLists)
		{
			if (auto* list = m_CommandLists.Get(handle))
				buffers.push_back(list->GetCommandBuffer());
		}
		TE_CORE_ASSERT(!buffers.empty(), "Submit requires at least one command list (v1)");

		uint64_t waitValue = 0;
		for (const TimelineWait& wait : waits)
			waitValue = std::max(waitValue, wait.Value);

		uint64_t signalValue = 0;
		for (const TimelineSignal& signal : signals)
			signalValue = std::max(signalValue, signal.Value);

		VkTimelineSemaphoreSubmitInfo timelineInfo{};
		timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timelineInfo.waitSemaphoreValueCount = waitValue != 0 ? 1u : 0u;
		timelineInfo.pWaitSemaphoreValues = &waitValue;
		timelineInfo.signalSemaphoreValueCount = 1u;
		timelineInfo.pSignalSemaphoreValues = &signalValue;

		VkSemaphore timelineSemaphore = m_Vulkan.GetTimelineSemaphore();
		// Timeline-semaphore waits must carry stage mask 0 (the wait is not
		// scoped to any pipeline stage).
		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_NONE;

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pNext = &timelineInfo;
		submitInfo.waitSemaphoreCount = timelineInfo.waitSemaphoreValueCount;
		submitInfo.pWaitSemaphores = &timelineSemaphore;
		submitInfo.pWaitDstStageMask = &waitStage;
		submitInfo.commandBufferCount = (uint32_t)buffers.size();
		submitInfo.pCommandBuffers = buffers.data();
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &timelineSemaphore;

		// The value allocation + the vkQueueSubmit share one critical
		// section on the queue mutex: concurrent allocators (two windows'
		// frame submits, the asset upload batch) must execute in allocation
		// order (VUID-VkSubmitInfo-pSignalSemaphores-03242).
		{
			std::lock_guard queueLock(m_Vulkan.GetQueueMutex());
			if (signalValue == 0)
				signalValue = m_Vulkan.AllocateTimelineValue();
			else
			{
				// An explicit signal must outrun every wait in this submit
				// (the same semaphore is both waited and signalled) and
				// keep the device's allocator monotonic for later submits.
				TE_CORE_ASSERT(signalValue > waitValue, "a submit must signal past every timeline value it waits on");
				m_Vulkan.AdvanceTimelineValue(signalValue);
			}
			VulkanCheckResult(vkQueueSubmit(m_Vulkan.GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE),
				"vkQueueSubmit", __FILE__, __LINE__);
		}
		return signalValue;
	}

	void VulkanRhiDevice::WaitIdle()
	{
		if (m_Initialized)
		{
			// F2: scope the device-wait under the queue mutex (live submit
			// threads on the same queue - the wait covers ONLY this).
			std::lock_guard queueLock(m_Vulkan.GetQueueMutex());
			vkDeviceWaitIdle(m_Vulkan.GetDevice());
		}
	}

	SwapchainHandle VulkanRhiDevice::CreateSwapchain(void* window)
	{
		// P-A2: wraps the per-window swapchain (now timeline-paced - the
		// per-frame fences are gone). VulkanContext still drives the frame
		// loop through its own VulkanSwapchain instance; this is the RHI's
		// owner API for per-window state (P-D's WispWindowRenderState).
		EnsureDevice((GLFWwindow*)window);
		SwapchainHandle handle = m_Swapchains.Acquire();
		m_Swapchains.Publish(handle, std::make_unique<VulkanSwapchain>());
		m_Swapchains.Get(handle)->Init(m_Vulkan, (GLFWwindow*)window);
		return handle;
	}

	void VulkanRhiDevice::DestroySwapchain(SwapchainHandle handle)
	{
		if (VulkanSwapchain* swapchain = m_Swapchains.Get(handle))
			swapchain->Shutdown();
		m_Swapchains.Release(handle);
	}

	// ---- Internal handle resolution ------------------------------------------------

	VkBuffer VulkanRhiDevice::GetBufferHandle(BufferHandle handle) const
	{
		// The field read happens under the table's shared lock: the wrapper
		// may be freed by a concurrent game-thread Release the instant the
		// lock drops (the record workers read these during record).
		// A Retiring slot still resolves (the record of a frame that drew it
		// may run after its owner died).
		std::shared_lock lock(m_Buffers.Mutex);
		if (const VulkanBuffer* buffer = m_Buffers.GetResolvableNoLock(handle))
			return buffer->GetHandle();
		return VK_NULL_HANDLE;
	}

	VkImage VulkanRhiDevice::GetTextureImage(TextureHandle handle) const
	{
		// Wrapped entries carry no owned image (the caller owns it) - but the
		// adopted image is recorded so barrier targets resolve through the
		// same handle (P4-B). Read under the table's shared lock.
		std::shared_lock lock(m_Textures.Mutex);
		if (const VulkanTextureEntry* entry = m_Textures.GetNoLock(handle))
		{
			if (entry->Owned)
				return entry->Owned->GetImage();
			if (entry->Wrapped)
				return entry->Wrapped->GetImage();
		}
		return VK_NULL_HANDLE;
	}

	VkImageView VulkanRhiDevice::GetTextureView(TextureHandle handle) const
	{
		std::shared_lock lock(m_Textures.Mutex);
		if (const VulkanTextureEntry* entry = m_Textures.GetNoLock(handle))
		{
			if (entry->Owned)
				return entry->Owned->GetImageView();
		}
		return VK_NULL_HANDLE;
	}

	VkExtent2D VulkanRhiDevice::GetTextureSize(TextureHandle handle) const
	{
		std::shared_lock lock(m_Textures.Mutex);
		if (const VulkanTextureEntry* entry = m_Textures.GetNoLock(handle))
		{
			if (entry->Owned)
				return { entry->Owned->GetWidth(), entry->Owned->GetHeight() };
		}
		return { 1, 1 };
	}

	VkPipeline VulkanRhiDevice::GetPipelineHandle(PipelineHandle handle) const
	{
		std::shared_lock lock(m_Pipelines.Mutex);
		if (const VulkanPipeline* pipeline = m_Pipelines.GetNoLock(handle))
			return pipeline->GetHandle();
		return VK_NULL_HANDLE;
	}

	VkPipelineLayout VulkanRhiDevice::GetPipelineLayout(PipelineHandle handle) const
	{
		std::shared_lock lock(m_Pipelines.Mutex);
		if (const VulkanPipeline* pipeline = m_Pipelines.GetNoLock(handle))
			return pipeline->GetLayout();
		return VK_NULL_HANDLE;
	}

} // namespace rhi
} // namespace TinyEngine
