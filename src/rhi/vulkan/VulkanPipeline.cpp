#include "tepch.h"
#include "Platform/Vulkan/VulkanPipeline.h"

namespace TinyEngine {

	void VulkanPipeline::Init(VulkanDevice& device, const VulkanPipelineSpec& spec, const std::string& debugName)
	{
		m_Device = &device;

		VkPushConstantRange pushConstantRange{};
		pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pushConstantRange.offset = 0;
		pushConstantRange.size = spec.PushConstantSize;

		VkPipelineLayoutCreateInfo layoutCreateInfo{};
		layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutCreateInfo.setLayoutCount = spec.DescriptorSetLayout != VK_NULL_HANDLE ? 1u : 0u;
		layoutCreateInfo.pSetLayouts = &spec.DescriptorSetLayout;
		layoutCreateInfo.pushConstantRangeCount = spec.PushConstantSize > 0 ? 1u : 0u;
		layoutCreateInfo.pPushConstantRanges = &pushConstantRange;

		VK_CHECK(vkCreatePipelineLayout(device.GetDevice(), &layoutCreateInfo, nullptr, &m_Layout));
		device.SetObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, (uint64_t)m_Layout, debugName + " Layout");

		VkPipelineShaderStageCreateInfo stages[2] = {};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = spec.VertexShader->GetStage();
		stages[0].module = spec.VertexShader->GetHandle();
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = spec.FragmentShader->GetStage();
		stages[1].module = spec.FragmentShader->GetHandle();
		stages[1].pName = "main";

		VkVertexInputBindingDescription bindingDescription{};
		bindingDescription.binding = 0;
		bindingDescription.stride = spec.VertexStride;
		bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		std::vector<VkVertexInputAttributeDescription> attributeDescriptions(spec.VertexAttributes.size());
		for (size_t i = 0; i < spec.VertexAttributes.size(); i++)
		{
			attributeDescriptions[i].location = spec.VertexAttributes[i].Location;
			attributeDescriptions[i].binding = 0;
			attributeDescriptions[i].format = spec.VertexAttributes[i].Format;
			attributeDescriptions[i].offset = spec.VertexAttributes[i].Offset;
		}

		VkPipelineVertexInputStateCreateInfo vertexInputState{};
		vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputState.vertexBindingDescriptionCount = 1;
		vertexInputState.pVertexBindingDescriptions = &bindingDescription;
		vertexInputState.vertexAttributeDescriptionCount = (uint32_t)attributeDescriptions.size();
		vertexInputState.pVertexAttributeDescriptions = attributeDescriptions.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
		inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		// Viewport/scissor are dynamic state (set per frame via vkCmdSetViewport/
		// vkCmdSetScissor) so the pipeline doesn't need rebuilding on resize.
		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo rasterizationState{};
		rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
		// No culling: this is a flat, camera facing 2D quad test, not worth
		// tying correctness to getting winding order exactly right.
		rasterizationState.cullMode = VK_CULL_MODE_NONE;
		rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizationState.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisampleState{};
		multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depthStencilState{};
		depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencilState.depthTestEnable = spec.DepthTestEnable ? VK_TRUE : VK_FALSE;
		depthStencilState.depthWriteEnable = spec.DepthWriteEnable ? VK_TRUE : VK_FALSE;
		depthStencilState.depthCompareOp = spec.DepthCompareOp;

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blendAttachment.blendEnable = spec.EnableBlend ? VK_TRUE : VK_FALSE;
		if (spec.EnableBlend)
		{
			// Matches OpenGLRendererAPI::Init's glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
			blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
			blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
			if (spec.PreserveDestinationAlpha)
			{
				// Leave the destination alpha untouched (outline composite):
				// the colour still blends normally, but the alpha channel of
				// the already-rendered opaque display target stays 1.0, so
				// ImGui's own SRC_ALPHA blending of that texture doesn't show
				// the window background through the non-outline pixels.
				blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
				blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
			}
			else
			{
				blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
			}
			blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
		}

		VkPipelineColorBlendStateCreateInfo colorBlendState{};
		colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlendState.attachmentCount = 1;
		colorBlendState.pAttachments = &blendAttachment;

		VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynamicStates;

		// No VkRenderPass/VkFramebuffer, this pipeline is built against the
		// dynamic rendering attachment formats directly (VK_KHR_dynamic_rendering
		// is core in 1.3).
		VkPipelineRenderingCreateInfo renderingCreateInfo{};
		renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		renderingCreateInfo.colorAttachmentCount = 1;
		renderingCreateInfo.pColorAttachmentFormats = &spec.ColorAttachmentFormat;
		renderingCreateInfo.depthAttachmentFormat = spec.DepthAttachmentFormat;

		VkGraphicsPipelineCreateInfo pipelineCreateInfo{};
		pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCreateInfo.pNext = &renderingCreateInfo;
		pipelineCreateInfo.stageCount = 2;
		pipelineCreateInfo.pStages = stages;
		pipelineCreateInfo.pVertexInputState = &vertexInputState;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.layout = m_Layout;

		VK_CHECK(vkCreateGraphicsPipelines(device.GetDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &m_Pipeline));
		device.SetObjectName(VK_OBJECT_TYPE_PIPELINE, (uint64_t)m_Pipeline, debugName);
	}

	void VulkanPipeline::Shutdown()
	{
		if (m_Pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(m_Device->GetDevice(), m_Pipeline, nullptr);
		if (m_Layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(m_Device->GetDevice(), m_Layout, nullptr);

		m_Pipeline = VK_NULL_HANDLE;
		m_Layout = VK_NULL_HANDLE;
	}

}
