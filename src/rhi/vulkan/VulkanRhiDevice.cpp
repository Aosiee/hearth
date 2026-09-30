#include "tepch.h"
#include "Platform/Vulkan/RHI/VulkanRhiDevice.h"

#include "TinyEngine/Core/Log.h"

#include <algorithm>

namespace TinyEngine {
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

		VkFormat ToVkFormat(Format format)
		{
			switch (format)
			{
			case Format::B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::B8G8R8A8_SRGB:  return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
			case Format::R8_UNORM:       return VK_FORMAT_R8_UNORM;
			case Format::D32_SFLOAT:     return VK_FORMAT_D32_SFLOAT;
			case Format::Undefined:      return VK_FORMAT_UNDEFINED;
			}
			return VK_FORMAT_UNDEFINED;
		}

		VkImageLayout ToVkLayout(TextureState state)
		{
			switch (state)
			{
			case TextureState::Undefined:    return VK_IMAGE_LAYOUT_UNDEFINED;
			case TextureState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case TextureState::DepthWrite:   return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case TextureState::DepthRead:    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			case TextureState::ShaderRead:   return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case TextureState::TransferSrc:  return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case TextureState::TransferDst:  return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case TextureState::Present:      return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
			case TextureState::DepthRead:    return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			case TextureState::ShaderRead:   return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			case TextureState::TransferSrc:  return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			case TextureState::TransferDst:  return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			case TextureState::Present:      return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
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
			case TextureState::DepthRead:    return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			case TextureState::ShaderRead:   return VK_ACCESS_2_SHADER_READ_BIT;
			case TextureState::TransferSrc:  return VK_ACCESS_2_TRANSFER_READ_BIT;
			case TextureState::TransferDst:  return VK_ACCESS_2_TRANSFER_WRITE_BIT;
			case TextureState::Present:      return VK_ACCESS_2_NONE;
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
	// device. Command buffers come from the device's (frame, thread)-keyed
	// pools; v1 records on thread 0.

	VulkanCommandList::VulkanCommandList(VulkanRhiDevice* device, VkCommandBuffer commandBuffer)
		: m_Device(device), m_CommandBuffer(commandBuffer)
	{
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
		VkRenderingAttachmentInfo colourAttachment{};
		VkRenderingAttachmentInfo depthAttachment{};

		for (const AttachmentDesc& attachment : attachments)
		{
			if (IsValid(attachment.Color))
			{
				colourAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				colourAttachment.imageView = m_Device->GetTextureView(attachment.Color);
				colourAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				colourAttachment.loadOp = attachment.LoadColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				colourAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				const float* clear = m_Device->GetClearColour();
				colourAttachment.clearValue.color = { { clear[0], clear[1], clear[2], clear[3] } };
				renderingInfo.colorAttachmentCount = 1;
				renderingInfo.pColorAttachments = &colourAttachment;
				renderingInfo.renderArea.extent = m_Device->GetTextureSize(attachment.Color);
			}
			if (IsValid(attachment.Depth))
			{
				depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				depthAttachment.imageView = m_Device->GetTextureView(attachment.Depth);
				depthAttachment.imageLayout = attachment.DepthReadOnly
					? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
					: VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depthAttachment.loadOp = attachment.LoadDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				depthAttachment.clearValue.depthStencil = { 1.0f, 0 };
				renderingInfo.pDepthAttachment = &depthAttachment;
			}
		}
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
					|| t.To == TextureState::DepthWrite || t.To == TextureState::DepthRead;
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

	void VulkanCommandList::BeginLabel(const char* name)
	{
		m_Device->GetVulkan()->CmdBeginDebugLabel(m_CommandBuffer, name);
	}

	void VulkanCommandList::EndLabel()
	{
		m_Device->GetVulkan()->CmdEndDebugLabel(m_CommandBuffer);
	}

	void VulkanCommandList::WriteTimestamp(uint32_t queryIndex)
	{
		// The device's timestamp pools are frame-indexed; the RHI command
		// list doesn't know its frame slot yet, so P-C (the port that
		// sources GpuFrameDebugData through WriteTimestamp) threads the
		// frame index in. Until then this writes the frame-0 pool.
		vkCmdWriteTimestamp(m_CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			m_Device->GetVulkan()->GetTimestampPool(0), queryIndex);
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
		TE_CORE_ASSERT(windowForExtensionQuery, "the RHI device must be initialised by a window (instance-extension query)");
		m_Vulkan.Init(windowForExtensionQuery);
		m_Initialized = true;
		m_GraphicsQueue.Index = 0;
		m_GraphicsQueue.Generation = 0;
	}

	void VulkanRhiDevice::ShutdownDevice()
	{
		if (!m_Initialized)
			return;
		m_Swapchains.Clear();
		m_CommandLists.Clear();
		m_Pipelines.Clear();
		m_Shaders.Clear();
		m_Samplers.Clear();
		m_Textures.Clear();
		m_Buffers.Clear();
		m_Vulkan.Shutdown();
		m_Initialized = false;
	}

	// ---- Buffer -----------------------------------------------------------------

	BufferHandle VulkanRhiDevice::CreateBuffer(const BufferDesc& desc, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI buffer created before the device");
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
			| VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (desc.StorageUsage)
			usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

		BufferHandle handle = m_Buffers.Acquire();
		VulkanBuffer* buffer = new VulkanBuffer();
		buffer->Init(m_Vulkan, desc.Size, usage, desc.HostVisible, debugName);
		m_Buffers.Slots[handle.Index].Object.reset(buffer);
		return handle;
	}

	void VulkanRhiDevice::DestroyBuffer(BufferHandle handle)
	{
		if (VulkanBuffer* buffer = m_Buffers.Get(handle))
			buffer->Shutdown();
		TE_CORE_ASSERT(m_Buffers.Release(handle), "destroying an invalid/stale RHI buffer");
	}

	void VulkanRhiDevice::SetBufferData(BufferHandle handle, const void* data, uint64_t size, uint64_t offset)
	{
		if (VulkanBuffer* buffer = m_Buffers.Get(handle))
			buffer->SetData(data, size, offset);
	}

	void VulkanRhiDevice::UploadBufferViaStaging(BufferHandle handle, const void* data, uint64_t size)
	{
		if (VulkanBuffer* buffer = m_Buffers.Get(handle))
			buffer->UploadViaStaging(data, size);
	}

	// ---- Texture ------------------------------------------------------------------

	TextureHandle VulkanRhiDevice::CreateTexture(const TextureDesc& desc, const void* initialData, uint64_t dataSize, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI texture created before the device");
		TextureHandle handle = m_Textures.Acquire();
		VulkanTexture2D* texture = new VulkanTexture2D(desc.Width, desc.Height);
		if (initialData && dataSize > 0)
			texture->SetData(const_cast<void*>(initialData), (uint32_t)dataSize);
		m_Textures.Slots[handle.Index].Object.reset(texture);
		return handle;
	}

	TextureHandle VulkanRhiDevice::CreateTextureFromBakedMips(uint32_t width, uint32_t height,
		const std::vector<std::vector<uint8_t>>& mipLevels, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI texture created before the device");
		TextureHandle handle = m_Textures.Acquire();
		m_Textures.Slots[handle.Index].Object.reset(new VulkanTexture2D(width, height, mipLevels));
		return handle;
	}

	void VulkanRhiDevice::DestroyTexture(TextureHandle handle)
	{
		TE_CORE_ASSERT(m_Textures.Release(handle), "destroying an invalid/stale RHI texture");
	}

	void VulkanRhiDevice::SetTextureData(TextureHandle handle, const void* data, uint64_t size)
	{
		if (VulkanTexture2D* texture = m_Textures.Get(handle))
			texture->SetData(const_cast<void*>(data), (uint32_t)size);
	}

	void VulkanRhiDevice::UpdateTextureBakedMips(TextureHandle handle, const std::vector<std::vector<uint8_t>>& mipLevels)
	{
		if (VulkanTexture2D* texture = m_Textures.Get(handle))
			texture->UpdateBakedMips(texture->GetWidth(), texture->GetHeight(), mipLevels);
	}

	// ---- Sampler / Shader / Pipeline ----------------------------------------------

	SamplerHandle VulkanRhiDevice::CreateSampler(const SamplerDesc& desc, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI sampler created before the device");
		SamplerHandle handle = m_Samplers.Acquire();
		VulkanSampler* sampler = new VulkanSampler();
		sampler->Init(m_Vulkan, desc, debugName);
		m_Samplers.Slots[handle.Index].Object.reset(sampler);
		return handle;
	}

	void VulkanRhiDevice::DestroySampler(SamplerHandle handle)
	{
		if (VulkanSampler* sampler = m_Samplers.Get(handle))
			sampler->Shutdown();
		TE_CORE_ASSERT(m_Samplers.Release(handle), "destroying an invalid/stale RHI sampler");
	}

	ShaderHandle VulkanRhiDevice::CreateShader(std::span<const uint32_t> spirv, ShaderStage stage, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI shader created before the device");
		ShaderHandle handle = m_Shaders.Acquire();
		VulkanShaderModule* shader = new VulkanShaderModule();
		shader->Init(m_Vulkan, std::vector<uint32_t>(spirv.begin(), spirv.end()), ToVkStage(stage), debugName);
		m_Shaders.Slots[handle.Index].Object.reset(shader);
		return handle;
	}

	void VulkanRhiDevice::DestroyShader(ShaderHandle handle)
	{
		if (VulkanShaderModule* shader = m_Shaders.Get(handle))
			shader->Shutdown();
		TE_CORE_ASSERT(m_Shaders.Release(handle), "destroying an invalid/stale RHI shader");
	}

	PipelineHandle VulkanRhiDevice::CreatePipeline(const PipelineStateDesc& desc, const char* debugName)
	{
		TE_CORE_ASSERT(m_Initialized, "RHI pipeline created before the device");

		VulkanPipelineSpec spec;
		spec.VertexShader = m_Shaders.Get(desc.VertexShader);
		spec.FragmentShader = m_Shaders.Get(desc.FragmentShader);
		spec.VertexStride = desc.VertexStride;
		for (const auto& attribute : desc.VertexAttributes)
			spec.VertexAttributes.push_back({ attribute.Location, ToVkFormat(attribute.PixelFormat), attribute.Offset });
		spec.EnableBlend = desc.EnableBlend;
		spec.PreserveDestinationAlpha = desc.PreserveDestinationAlpha;
		spec.DepthTestEnable = desc.DepthTestEnable;
		spec.DepthWriteEnable = desc.DepthWriteEnable;
		spec.ColorAttachmentFormat = ToVkFormat(desc.ColorAttachmentFormat);
		spec.DepthAttachmentFormat = ToVkFormat(desc.DepthAttachmentFormat);
		spec.PushConstantSize = desc.PushConstantSize;
		spec.PushConstantStages = desc.PushConstantStages;
		// P-A1: no descriptor set layout (the global bindless set lands in
		// P-A3; the renderers' own pipelines still use their layouts).

		PipelineHandle handle = m_Pipelines.Acquire();
		VulkanPipeline* pipeline = new VulkanPipeline();
		pipeline->Init(m_Vulkan, spec, debugName);
		m_Pipelines.Slots[handle.Index].Object.reset(pipeline);
		return handle;
	}

	void VulkanRhiDevice::DestroyPipeline(PipelineHandle handle)
	{
		if (VulkanPipeline* pipeline = m_Pipelines.Get(handle))
			pipeline->Shutdown();
		TE_CORE_ASSERT(m_Pipelines.Release(handle), "destroying an invalid/stale RHI pipeline");
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
		m_CommandLists.Slots[handle.Index].Object.reset(new VulkanCommandList(this, commandBuffer));
		return handle;
	}

	void VulkanRhiDevice::DestroyCommandList(CommandListHandle handle)
	{
		TE_CORE_ASSERT(m_CommandLists.Release(handle), "destroying an invalid/stale RHI command list");
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
		if (signalValue == 0)
			signalValue = m_Vulkan.AllocateTimelineValue();
		else
		{
			// An explicit signal must outrun every wait in this submit (the
			// same semaphore is both waited and signalled) and keep the
			// device's allocator monotonic for later submits.
			TE_CORE_ASSERT(signalValue > waitValue, "a submit must signal past every timeline value it waits on");
			m_Vulkan.AdvanceTimelineValue(signalValue);
		}

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

		VulkanCheckResult(vkQueueSubmit(m_Vulkan.GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE),
			"vkQueueSubmit", __FILE__, __LINE__);
		return signalValue;
	}

	void VulkanRhiDevice::WaitIdle()
	{
		if (m_Initialized)
			vkDeviceWaitIdle(m_Vulkan.GetDevice());
	}

	SwapchainHandle VulkanRhiDevice::CreateSwapchain(void* window)
	{
		// P-A2: wraps the per-window swapchain (now timeline-paced - the
		// per-frame fences are gone). VulkanContext still drives the frame
		// loop through its own VulkanSwapchain instance; this is the RHI's
		// owner API for per-window state (P-D's WispWindowRenderState).
		EnsureDevice((GLFWwindow*)window);
		SwapchainHandle handle = m_Swapchains.Acquire();
		m_Swapchains.Slots[handle.Index].Object = std::make_unique<VulkanSwapchain>();
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
		if (const VulkanBuffer* buffer = m_Buffers.Get(handle))
			return buffer->GetHandle();
		return VK_NULL_HANDLE;
	}

	VkImage VulkanRhiDevice::GetTextureImage(TextureHandle handle) const
	{
		if (const VulkanTexture2D* texture = m_Textures.Get(handle))
			return texture->GetImage();
		return VK_NULL_HANDLE;
	}

	VkImageView VulkanRhiDevice::GetTextureView(TextureHandle handle) const
	{
		if (const VulkanTexture2D* texture = m_Textures.Get(handle))
			return texture->GetImageView();
		return VK_NULL_HANDLE;
	}

	VkExtent2D VulkanRhiDevice::GetTextureSize(TextureHandle handle) const
	{
		if (const VulkanTexture2D* texture = m_Textures.Get(handle))
			return { texture->GetWidth(), texture->GetHeight() };
		return { 1, 1 };
	}

	VkPipeline VulkanRhiDevice::GetPipelineHandle(PipelineHandle handle) const
	{
		if (const VulkanPipeline* pipeline = m_Pipelines.Get(handle))
			return pipeline->GetHandle();
		return VK_NULL_HANDLE;
	}

	VkPipelineLayout VulkanRhiDevice::GetPipelineLayout(PipelineHandle handle) const
	{
		if (const VulkanPipeline* pipeline = m_Pipelines.Get(handle))
			return pipeline->GetLayout();
		return VK_NULL_HANDLE;
	}

} // namespace rhi
} // namespace TinyEngine
