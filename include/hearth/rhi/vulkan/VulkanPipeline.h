#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanShaderModule.h"

namespace TinyEngine {

	struct VulkanVertexAttribute
	{
		uint32_t Location;
		VkFormat Format;
		uint32_t Offset;
	};

	// Everything Vulkan needs baked into a pipeline object up front: shader
	// stages, vertex layout, and fixed function state (blend/depth/raster).
	// State that was runtime toggles in OpenGL (glEnable(GL_BLEND) etc, see
	// RendererAPI::Init) has to be known at pipeline creation time here
	// instead.
	//
	// Depth support (DepthTestEnable/DepthWriteEnable/DepthCompareOp/
	// DepthAttachmentFormat below) exists for Renderer3D's depth-tested
	// geometry - Renderer2D's own pipeline still leaves depth test/write
	// off, but does declare a matching DepthAttachmentFormat, since it
	// shares a render target with Renderer3D's depth-tested geometry (see
	// VulkanCommon.h's kSharedDepthFormat for why that has to match
	// exactly).
	struct VulkanPipelineSpec
	{
		VulkanShaderModule* VertexShader = nullptr;
		VulkanShaderModule* FragmentShader = nullptr;

		uint32_t VertexStride = 0;
		std::vector<VulkanVertexAttribute> VertexAttributes;

		VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;

		// Vertex stage only for now (a single MVP matrix), see VulkanPipeline.cpp.
		uint32_t PushConstantSize = 0;

		VkFormat ColorAttachmentFormat = VK_FORMAT_UNDEFINED;
		bool EnableBlend = true;

		// When true, the alpha channel is
		// blended with (ZERO, ONE) instead of the default (ONE, ZERO), i.e.
		// the destination alpha is left completely untouched. The outline
		// composite pass needs this: it overlays an opaque outline over an
		// already-rendered opaque image (the viewport framebuffer or a debug
		// framebuffer) whose alpha is already 1.0 everywhere, and the default
		// alpha blend would otherwise overwrite that alpha with the
		// composite's own output alpha - which is 0 for non-edge pixels
		// (vec4(0.0)), making the whole viewport texture transparent to
		// ImGui's SRC_ALPHA blending and showing the window's grey background
		// through everything except the outline.
		bool PreserveDestinationAlpha = false;

		// All default to off. DepthAttachmentFormat
		// still needs setting even when DepthTestEnable/DepthWriteEnable are both
		// false, as long as the target this pipeline draws into has a depth
		// attachment active - the format declared here must match the active
		// rendering instance's depth format exactly or it's a validation error
		// the moment this pipeline is bound, regardless of whether it actually
		// tests or writes depth.
		bool DepthTestEnable = false;
		bool DepthWriteEnable = false;
		VkCompareOp DepthCompareOp = VK_COMPARE_OP_LESS;
		VkFormat DepthAttachmentFormat = VK_FORMAT_UNDEFINED;
	};

	class VulkanPipeline
	{
	public:
		void Init(VulkanDevice& device, const VulkanPipelineSpec& spec, const std::string& debugName);
		void Shutdown();

		VkPipeline GetHandle() const { return m_Pipeline; }
		VkPipelineLayout GetLayout() const { return m_Layout; }

	private:
		VulkanDevice* m_Device = nullptr;
		VkPipelineLayout m_Layout = VK_NULL_HANDLE;
		VkPipeline m_Pipeline = VK_NULL_HANDLE;
	};

}
