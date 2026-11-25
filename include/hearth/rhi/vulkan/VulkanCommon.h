#pragma once

// volk loads Vulkan at run time, so nothing links against the system
// Vulkan loader and no Vulkan SDK is needed to build. It also brings in
// Vulkan-Headers.
#include <volk.h>

// VMA is told to fetch every Vulkan function it needs itself, dynamically, starting
// from just vkGetInstanceProcAddr/vkGetDeviceProcAddr (handed to it from volk when
// the allocator is created). That means VMA is never linked against the loader directly,
// matching how the rest of the backend talks to Vulkan through volk.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
// The vendored header is not warning-clean under -Wall -Wextra
// (silenced here - one site covers every Vulkan TU). Clang also
// reports its partial _Nullable/_Nonnull annotations and an unused
// placeholder member.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#include <vk_mem_alloc.h>
#pragma GCC diagnostic pop

#include <string>

#include "hearth/Assert.h"
#include "hearth/Log.h"

namespace hearth {

	// The GPU layer's log category: validation messages, device and
	// swapchain lifetime, failed Vulkan calls.
	HEARTH_LOG_CATEGORY(LogVulkan)

}

namespace hearth::rhi {

	// The one depth format of every target the layer makes (swapchain and
	// framebuffers alike): a pipeline's depthAttachmentFormat must match the
	// target it is bound against, even with depth test and write off, so one
	// format lets one pipeline draw into any of them. D32_SFLOAT is not
	// guaranteed by the specification, but every desktop GPU supports it.
	constexpr VkFormat kSharedDepthFormat = VK_FORMAT_D32_SFLOAT;

	// A failed Vulkan call is an assertion, not a half-made device or
	// swapchain carrying on. Every call that returns a VkResult goes
	// through VK_CHECK.
	inline void VulkanCheckResult(VkResult result, const char* expression, const char* file, int line)
	{
		if (result != VK_SUCCESS)
		{
			HEARTH_LOG_ERROR(::hearth::LogVulkan, "Vulkan call failed: {0} returned {1} ({2}:{3})", expression, (int)result, file, line);
			HEARTH_ASSERT(false, "Vulkan call failed, see error above.");
		}
	}

}

#define VK_CHECK(expression) ::hearth::rhi::VulkanCheckResult((expression), #expression, __FILE__, __LINE__)
