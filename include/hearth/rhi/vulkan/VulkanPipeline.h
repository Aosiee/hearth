#pragma once

#include "Platform/Vulkan/VulkanCommon.h"
#include "Platform/Vulkan/Device/VulkanDevice.h"
#include "Platform/Vulkan/Shaders/VulkanShaderModule.h"

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
	// The pipeline description) has to be known at pipeline creation time here
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

		// The pipeline's single push-constant range. Vertex stage only by
		// default (a single MVP matrix), see VulkanPipeline.cpp. P4's overlay
		// discard mode needs the FRAGMENT stage to read a value too: GLSL
		// push-constant blocks always begin at offset 0 per stage, so the two
		// stages' blocks would overlap in the push-constant buffer. The clean
		// resolution is ONE range whose size covers the shared block
		// (e.g. mat4 + uint = 68 bytes) and whose stageFlags cover BOTH
		// stages - set PushConstantStages accordingly. Every vkCmdPushConstants
		// against that layout then uses the combined stage flags.
		uint32_t PushConstantSize = 0;
		VkShaderStageFlags PushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;

		VkFormat ColorAttachmentFormats[8] = {};
		uint32_t ColorAttachmentCount = 1;
		bool EnableBlend = true;
		// The colour-blend factors (mapped from rhi::BlendFactor in
		// VulkanRhiDevice::CreatePipeline; default = the classic alpha blend).
		VkBlendFactor ColorBlendSrc = VK_BLEND_FACTOR_SRC_ALPHA;
		VkBlendFactor ColorBlendDst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

		// When true, the alpha channel is
		// blended with (ZERO, ONE) instead of the default (ONE, ZERO), i.e.
		// the destination alpha is left completely untouched. The outline
		// composite pass needs this: it overlays an opaque outline over an
		// already-rendered opaque image (the viewport framebuffer or a debug
		// framebuffer) whose alpha is already 1.0 everywhere, and the default
		// alpha blend would otherwise overwrite that alpha with the
		// composite's own output alpha - which is 0 for non-edge pixels
		// (vec4(0.0)), making the whole viewport texture transparent to
		// the UI layer's SRC_ALPHA blending and showing the window's grey background
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
		// The hardware slope-scaled depth bias (dynamic state; the local
		// shadow caster's pipeline).
		bool DepthBiasEnable = false;
		float DepthBiasConstantFactor = 0.0f;
		float DepthBiasSlopeFactor = 0.0f;

		// FILL by default; the wireframe debug view's pipeline declares
		// LINE (baked at creation - fixed-function state).
		VkPolygonMode PolygonMode = VK_POLYGON_MODE_FILL;
	};

	class VulkanPipeline
	{
	public:
		// cache = the driver pipeline cache (VK_NULL_HANDLE = none).
		void Init(VulkanDevice& device, const VulkanPipelineSpec& spec, const std::string& debugName,
		          VkPipelineCache cache = VK_NULL_HANDLE);
		void Shutdown();

		VkPipeline GetHandle() const { return m_Pipeline; }
		VkPipelineLayout GetLayout() const { return m_Layout; }

		// The in-place rebuild's swap (`other` is left holding the old
		// objects for the retire queue).
		void SwapContents(VulkanPipeline& other)
		{
			std::swap(m_Pipeline, other.m_Pipeline);
			std::swap(m_Layout, other.m_Layout);
		}

	private:
		VulkanDevice* m_Device = nullptr;
		VkPipelineLayout m_Layout = VK_NULL_HANDLE;
		VkPipeline m_Pipeline = VK_NULL_HANDLE;
	};

}
