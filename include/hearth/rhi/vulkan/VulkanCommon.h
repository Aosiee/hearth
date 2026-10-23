#pragma once

// Volk is the vendored, dynamically loaded Vulkan meta-loader (third_party plugin).
// It is used instead of linking directly against the system Vulkan loader so the engine
// never has a build time dependency on the Vulkan SDK being installed. Including it here
// also pulls in the real Vulkan-Headers (vendored separately), which is what makes GLFW
// declare glfwCreateWindowSurface and friends when GLFW is included afterwards.
#include <volk.h>

// VMA is told to fetch every Vulkan function it needs itself, dynamically, starting
// from just vkGetInstanceProcAddr/vkGetDeviceProcAddr (handed to it from volk when
// the allocator is created). That means VMA is never linked against the loader directly,
// matching how the rest of the backend talks to Vulkan through volk.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
// The vendored header is not warning-clean under -Wall -Wextra
// (silenced here - one site covers every Vulkan TU).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <vk_mem_alloc.h>
#pragma GCC diagnostic pop

#include <string>

#include "TinyEngine/Core/Log.h"

namespace TinyEngine {

	// Shared by every Vulkan-side file that wants to log under its own
	// category instead of the generic Core logger - the validation
	// messenger callback (VulkanDevice.cpp) is the main user, since that's
	// the single noisiest source of log lines in the whole engine and
	// exactly the kind of thing you want to filter out of the log window
	// independently of everything else.
	TE_DECLARE_LOG_CATEGORY(LogVulkan)

	// Shared by VulkanSwapchain and VulkanFramebuffer - both targets need to
	// use the exact same depth format, not just any mutually-valid one,
	// because Renderer2D and Renderer3D each lazily create a single shared
	// pipeline object used against whichever target it's currently drawing
	// into (see VulkanRenderer2D.h's class comment for why one pipeline
	// serves both targets rather than one per target). A pipeline's
	// VkPipelineRenderingCreateInfo::depthAttachmentFormat has to match the
	// active rendering instance's depth attachment format exactly or it's a
	// validation error the moment that pipeline is bound, regardless of
	// whether depth test/write are even enabled on it - hardcoding one
	// format everywhere sidesteps needing per-target pipeline variants.
	// D32_SFLOAT isn't unconditionally guaranteed by the Vulkan spec the way
	// the swapchain's colour format effectively is on this hardware, but
	// every desktop GPU this engine has run on supports it - worth
	// revisiting with a real capability query if that ever changes.
	constexpr VkFormat kSharedDepthFormat = VK_FORMAT_D32_SFLOAT;

	// Turns a failed Vulkan call into an assertion instead of letting the backend limp
	// along with a half constructed device or swapchain. Every call that returns a
	// VkResult in the Vulkan backend should be wrapped in this.
	inline void VulkanCheckResult(VkResult result, const char* expression, const char* file, int line)
	{
		if (result != VK_SUCCESS)
		{
			TE_CORE_ERROR(LogVulkan, "Vulkan call failed: {0} returned {1} ({2}:{3})", expression, (int)result, file, line);
			TE_CORE_ASSERT(false, "Vulkan call failed, see error above.");
		}
	}

}

#define VK_CHECK(expression) ::TinyEngine::VulkanCheckResult((expression), #expression, __FILE__, __LINE__)
