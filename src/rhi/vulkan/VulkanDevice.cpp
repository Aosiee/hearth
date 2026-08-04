#include "tepch.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFrameContext.h"
#include "TinyEngine/Debug/GpuFrameDebugData.h"

#include <GLFW/glfw3.h>
#include <cstring>

namespace TinyEngine {

	namespace {

		// The one and only validation layer we care about. If it isn't present on
		// this machine (dev machine without the Vulkan SDK / LunarG layers
		// installed) we just carry on without it rather than refusing to start,
		// the same way the engine doesn't refuse to start without a debugger
		// attached.
		const char* g_ValidationLayerName = "VK_LAYER_KHRONOS_validation";

		// Kept separate from the log itself (Log.h has no ring buffer, and
		// giving it one just for this would be a bigger change than the GPU
		// frame debugger's validation panel actually needs) — capped so a
		// noisy run doesn't grow this forever.
		constexpr size_t kMaxBufferedValidationMessages = 50;

		void BufferValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message)
		{
			const char* severityText = "INFO";
			if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
				severityText = "ERROR";
			else if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
				severityText = "WARN";

			std::vector<std::string>& messages = GetMutableGpuFrameDebugInfo().ValidationMessages;
			messages.insert(messages.begin(), std::string("[") + severityText + "] " + message);
			if (messages.size() > kMaxBufferedValidationMessages)
				messages.resize(kMaxBufferedValidationMessages);
		}

		VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT type,
			const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
			void* userData)
		{
			(void)type;
			(void)userData;

			switch (severity)
			{
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
					TE_CORE_ERROR(LogVulkan, "{0}", callbackData->pMessage);
					break;
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
					TE_CORE_WARN(LogVulkan, "{0}", callbackData->pMessage);
					break;
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
					TE_CORE_INFO(LogVulkan, "{0}", callbackData->pMessage);
					break;
				default:
					TE_CORE_DEBUG(LogVulkan, "{0}", callbackData->pMessage);
					break;
			}

			BufferValidationMessage(severity, callbackData->pMessage);

			// Returning true here would abort the call that triggered the message,
			// which is almost never what you want outside of a targeted test.
			return VK_FALSE;
		}

	}

	void VulkanDevice::Init(GLFWwindow* windowForExtensionQuery)
	{
		TE_PROFILE_FUNCTION();

		VK_CHECK(volkInitialize());

		CreateInstance(windowForExtensionQuery);
		volkLoadInstance(m_Instance);

		if (m_ValidationEnabled)
			CreateDebugMessenger();

		PickPhysicalDevice();
		CreateLogicalDevice();
		volkLoadDevice(m_Device);

		CreateAllocator();
		CreateCommandPools();
		CreateTimestampQueryPools();
	}

	void VulkanDevice::Shutdown()
	{
		TE_PROFILE_FUNCTION();

		if (m_Device != VK_NULL_HANDLE)
			vkDeviceWaitIdle(m_Device);

		for (VkQueryPool pool : m_TimestampPools)
		{
			if (pool != VK_NULL_HANDLE)
				vkDestroyQueryPool(m_Device, pool, nullptr);
		}

		for (VkCommandPool pool : m_CommandPools)
		{
			if (pool != VK_NULL_HANDLE)
				vkDestroyCommandPool(m_Device, pool, nullptr);
		}

		if (m_TransientCommandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(m_Device, m_TransientCommandPool, nullptr);

		if (m_Allocator != VK_NULL_HANDLE)
			vmaDestroyAllocator(m_Allocator);

		if (m_Device != VK_NULL_HANDLE)
			vkDestroyDevice(m_Device, nullptr);

		if (m_DebugMessenger != VK_NULL_HANDLE)
			vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);

		if (m_Instance != VK_NULL_HANDLE)
			vkDestroyInstance(m_Instance, nullptr);
	}

	bool VulkanDevice::CheckValidationLayerSupport() const
	{
		uint32_t layerCount = 0;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

		std::vector<VkLayerProperties> availableLayers(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

		for (const VkLayerProperties& layer : availableLayers)
		{
			if (strcmp(layer.layerName, g_ValidationLayerName) == 0)
				return true;
		}

		return false;
	}

	void VulkanDevice::CreateInstance(GLFWwindow* windowForExtensionQuery)
	{
		(void)windowForExtensionQuery;

		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "TinyEngine App";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "TinyEngine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		// Vulkan 1.3 core is TinyEngine's confirmed baseline.
		// Dynamic rendering and synchronization2 are both used unconditionally as core
		// features rather than extensions further down this backend.
		appInfo.apiVersion = VK_API_VERSION_1_3;

		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
		TE_CORE_ASSERT(glfwExtensions, "GLFW could not provide the instance extensions Vulkan surface creation needs on this platform!");

		std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

#ifdef TE_ENABLE_ASSERTS
		m_ValidationEnabled = CheckValidationLayerSupport();
		if (m_ValidationEnabled)
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		else
			TE_CORE_WARN(LogVulkan, "VK_LAYER_KHRONOS_validation is not available on this machine, running without Vulkan validation.");
#endif

		VkInstanceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;
		createInfo.enabledExtensionCount = (uint32_t)extensions.size();
		createInfo.ppEnabledExtensionNames = extensions.data();

		// Chaining a messenger create info onto the instance create info means we also
		// catch validation messages raised by vkCreateInstance/vkDestroyInstance
		// themselves, which the messenger object created afterwards would otherwise miss.
		VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo{};
		if (m_ValidationEnabled)
		{
			const char* layers[] = { g_ValidationLayerName };
			createInfo.enabledLayerCount = 1;
			createInfo.ppEnabledLayerNames = layers;

			// Verbose/info would also surface the loader's own manifest discovery
			// chatter on every single run (which layers/drivers it found and where),
			// not just messages from the validation layer itself. Warnings and
			// errors are what we actually care about day to day.
			messengerCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			messengerCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messengerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messengerCreateInfo.pfnUserCallback = DebugMessengerCallback;

			createInfo.pNext = &messengerCreateInfo;
		}

		VK_CHECK(vkCreateInstance(&createInfo, nullptr, &m_Instance));

		TE_CORE_INFO(LogVulkan, "Vulkan Instance Created (validation {0})", m_ValidationEnabled ? "enabled" : "disabled");
	}

	void VulkanDevice::CreateDebugMessenger()
	{
		VkDebugUtilsMessengerCreateInfoEXT createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		createInfo.pfnUserCallback = DebugMessengerCallback;

		VK_CHECK(vkCreateDebugUtilsMessengerEXT(m_Instance, &createInfo, nullptr, &m_DebugMessenger));
	}

	void VulkanDevice::PickPhysicalDevice()
	{
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
		TE_CORE_ASSERT(deviceCount > 0, "No Vulkan capable physical devices found!");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

		VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
		uint32_t bestQueueFamily = 0;
		int bestScore = -1;

		for (VkPhysicalDevice device : devices)
		{
			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(device, &properties);

			// Vulkan 1.3 core is a hard requirement, not something we fall back
			// from.
			if (properties.apiVersion < VK_API_VERSION_1_3)
				continue;

			uint32_t queueFamilyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
			std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

			int suitableQueueFamily = -1;
			for (uint32_t i = 0; i < queueFamilyCount; i++)
			{
				bool hasGraphics = (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
				bool canPresent = glfwGetPhysicalDevicePresentationSupport(m_Instance, device, i) == GLFW_TRUE;

				if (hasGraphics && canPresent)
				{
					suitableQueueFamily = (int)i;
					break;
				}
			}

			if (suitableQueueFamily < 0)
				continue;

			int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 100;
			if (score > bestScore)
			{
				bestScore = score;
				bestDevice = device;
				bestQueueFamily = (uint32_t)suitableQueueFamily;
			}
		}

		TE_CORE_ASSERT(bestDevice != VK_NULL_HANDLE, "Could not find a physical device supporting Vulkan 1.3 with a graphics + present capable queue!");

		m_PhysicalDevice = bestDevice;
		m_GraphicsQueueFamily = bestQueueFamily;

		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
		TE_CORE_INFO(LogVulkan, "Vulkan Physical Device: {0}", properties.deviceName);

		// Needed to convert GPU timestamp query results (raw ticks) into
		// milliseconds for the GPU frame debugger. timestampValidBits == 0 on the
		// chosen queue family would mean timestamps aren't supported there at
		// all, rare but not impossible, checked rather than assumed the way
		// samplerAnisotropy is.
		m_TimestampPeriodNs = properties.limits.timestampPeriod;

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());
		m_TimestampsSupported = queueFamilies[m_GraphicsQueueFamily].timestampValidBits > 0;
	}

	void VulkanDevice::CreateLogicalDevice()
	{
		float queuePriority = 1.0f;
		VkDeviceQueueCreateInfo queueCreateInfo{};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = m_GraphicsQueueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;

		// Dynamic rendering and synchronization2 are Vulkan 1.3 core features rather
		// than extensions, but still need to be explicitly turned on per device.
		// Descriptor indexing and timeline semaphores (1.2 core) are enabled now too
		// even though nothing uses them yet, they're needed from Renderer2D's
		// bindless texture array onward.
		VkPhysicalDeviceVulkan13Features vulkan13Features{};
		vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13Features.dynamicRendering = VK_TRUE;
		vulkan13Features.synchronization2 = VK_TRUE;

		VkPhysicalDeviceVulkan12Features vulkan12Features{};
		vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		vulkan12Features.descriptorIndexing = VK_TRUE;
		vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
		vulkan12Features.descriptorBindingVariableDescriptorCount = VK_TRUE;
		vulkan12Features.runtimeDescriptorArray = VK_TRUE;
		vulkan12Features.timelineSemaphore = VK_TRUE;
		vulkan12Features.pNext = &vulkan13Features;

		// Anisotropic filtering is an optional feature (unlike GL, where a max
		// anisotropy limit is just always queryable), so it has to actually be
		// checked and requested rather than assumed on, or every sampler
		// created with anisotropyEnable = VK_TRUE is a validation error.
		VkPhysicalDeviceFeatures supportedFeatures;
		vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supportedFeatures);
		m_SamplerAnisotropySupported = supportedFeatures.samplerAnisotropy == VK_TRUE;

		VkPhysicalDeviceFeatures2 features2{};
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features2.pNext = &vulkan12Features;
		features2.features.samplerAnisotropy = m_SamplerAnisotropySupported ? VK_TRUE : VK_FALSE;

		const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		VkDeviceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.pNext = &features2;
		createInfo.queueCreateInfoCount = 1;
		createInfo.pQueueCreateInfos = &queueCreateInfo;
		createInfo.enabledExtensionCount = 1;
		createInfo.ppEnabledExtensionNames = deviceExtensions;
		// pEnabledFeatures is deliberately left null, the feature chain above (via
		// pNext) is how features are requested when using VkPhysicalDeviceFeatures2.

		VK_CHECK(vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device));

		vkGetDeviceQueue(m_Device, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);
	}

	void VulkanDevice::CreateAllocator()
	{
		VmaVulkanFunctions vulkanFunctions{};
		vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
		vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

		VmaAllocatorCreateInfo allocatorCreateInfo{};
		allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
		allocatorCreateInfo.physicalDevice = m_PhysicalDevice;
		allocatorCreateInfo.device = m_Device;
		allocatorCreateInfo.instance = m_Instance;
		allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

		VK_CHECK(vmaCreateAllocator(&allocatorCreateInfo, &m_Allocator));
	}

	void VulkanDevice::CreateCommandPools()
	{
		VkCommandPoolCreateInfo poolCreateInfo{};
		poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		// Individually resettable so each frame's pool can be reset in one call once
		// that frame's fence has signalled, rather than freeing/reallocating buffers.
		poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolCreateInfo.queueFamilyIndex = m_GraphicsQueueFamily;

		for (uint32_t i = 0; i < FramesInFlight; i++)
		{
			VK_CHECK(vkCreateCommandPool(m_Device, &poolCreateInfo, nullptr, &m_CommandPools[i]));
			SetObjectName(VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)m_CommandPools[i], "Frame In Flight Command Pool " + std::to_string(i));
		}

		VkCommandPoolCreateInfo transientPoolCreateInfo{};
		transientPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		// TRANSIENT tells the driver these buffers are short lived (allocated,
		// recorded, submitted, freed), which lets it use a cheaper allocation
		// strategy than the long lived per-frame pools.
		transientPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		transientPoolCreateInfo.queueFamilyIndex = m_GraphicsQueueFamily;

		VK_CHECK(vkCreateCommandPool(m_Device, &transientPoolCreateInfo, nullptr, &m_TransientCommandPool));
		SetObjectName(VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)m_TransientCommandPool, "Transient Upload Command Pool");
	}

	void VulkanDevice::CreateTimestampQueryPools()
	{
		if (!m_TimestampsSupported)
		{
			TE_CORE_WARN(LogVulkan, "This GPU's graphics queue doesn't report timestampValidBits, the GPU frame debugger's pass timings will stay unavailable.");
			return;
		}

		VkQueryPoolCreateInfo poolCreateInfo{};
		poolCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		poolCreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		poolCreateInfo.queryCount = kMaxTimestampsPerFrame;

		for (uint32_t i = 0; i < FramesInFlight; i++)
		{
			VK_CHECK(vkCreateQueryPool(m_Device, &poolCreateInfo, nullptr, &m_TimestampPools[i]));
			SetObjectName(VK_OBJECT_TYPE_QUERY_POOL, (uint64_t)m_TimestampPools[i], "GPU Frame Debugger Timestamp Pool " + std::to_string(i));

			// Query pools start in an undefined state, every slot needs an
			// explicit reset before its first use. Doing that once here via a
			// throwaway command buffer means the first real frame that uses
			// this slot doesn't need special-casing versus every frame after it.
			VkCommandBuffer cmd = BeginSingleTimeCommands();
			vkCmdResetQueryPool(cmd, m_TimestampPools[i], 0, kMaxTimestampsPerFrame);
			EndSingleTimeCommands(cmd);
		}
	}

	VkCommandBuffer VulkanDevice::BeginSingleTimeCommands() const
	{
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = m_TransientCommandPool;
		allocInfo.commandBufferCount = 1;

		VkCommandBuffer commandBuffer;
		VK_CHECK(vkAllocateCommandBuffers(m_Device, &allocInfo, &commandBuffer));

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

		return commandBuffer;
	}

	void VulkanDevice::EndSingleTimeCommands(VkCommandBuffer commandBuffer) const
	{
		VK_CHECK(vkEndCommandBuffer(commandBuffer));

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;

		VK_CHECK(vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
		// Simplest possible sync for a one-off upload: just wait for the whole
		// queue to drain. Fine for occasional resource loads, see the header
		// comment on why this isn't trying to be an async transfer system yet.
		VK_CHECK(vkQueueWaitIdle(m_GraphicsQueue));

		vkFreeCommandBuffers(m_Device, m_TransientCommandPool, 1, &commandBuffer);
	}

	void VulkanDevice::SetObjectName(VkObjectType type, uint64_t handle, const std::string& name) const
	{
		if (!m_ValidationEnabled || !vkSetDebugUtilsObjectNameEXT)
			return;

		VkDebugUtilsObjectNameInfoEXT nameInfo{};
		nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		nameInfo.objectType = type;
		nameInfo.objectHandle = handle;
		nameInfo.pObjectName = name.c_str();

		vkSetDebugUtilsObjectNameEXT(m_Device, &nameInfo);
	}

	void VulkanDevice::CmdBeginDebugLabel(VkCommandBuffer commandBuffer, const std::string& name, const float colour[4])
	{
		if (m_ValidationEnabled && vkCmdBeginDebugUtilsLabelEXT)
		{
			VkDebugUtilsLabelEXT label{};
			label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
			label.pLabelName = name.c_str();

			if (colour)
			{
				memcpy(label.color, colour, sizeof(float) * 4);
			}
			else
			{
				label.color[0] = label.color[1] = label.color[2] = label.color[3] = 1.0f;
			}

			vkCmdBeginDebugUtilsLabelEXT(commandBuffer, &label);
		}

		if (m_TimestampsSupported)
		{
			uint32_t frameIndex = GetVulkanCurrentFrameIndex();
			TE_CORE_ASSERT(m_NextQuerySlot[frameIndex] < kMaxTimestampsPerFrame, "GPU frame debugger ran out of timestamp query slots this frame, raise VulkanDevice::kMaxTimestampsPerFrame!");

			uint32_t slot = m_NextQuerySlot[frameIndex]++;
			vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_TimestampPools[frameIndex], slot);
			m_ScopeBeginStack[frameIndex].push_back(slot);
			m_ScopeNameStack[frameIndex].push_back(name);
		}
	}

	void VulkanDevice::CmdEndDebugLabel(VkCommandBuffer commandBuffer)
	{
		if (m_TimestampsSupported)
		{
			uint32_t frameIndex = GetVulkanCurrentFrameIndex();

			// Begin/EndDebugLabel calls are always properly paired at every
			// call site in this codebase, but guard anyway rather than pop an
			// empty stack if that's ever not true.
			if (!m_ScopeBeginStack[frameIndex].empty())
			{
				TE_CORE_ASSERT(m_NextQuerySlot[frameIndex] < kMaxTimestampsPerFrame, "GPU frame debugger ran out of timestamp query slots this frame, raise VulkanDevice::kMaxTimestampsPerFrame!");

				uint32_t endSlot = m_NextQuerySlot[frameIndex]++;
				vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_TimestampPools[frameIndex], endSlot);

				uint32_t beginSlot = m_ScopeBeginStack[frameIndex].back();
				m_ScopeBeginStack[frameIndex].pop_back();
				std::string scopeName = m_ScopeNameStack[frameIndex].back();
				m_ScopeNameStack[frameIndex].pop_back();

				m_RecordedScopes[frameIndex].push_back({ scopeName, beginSlot, endSlot });
			}
		}

		if (m_ValidationEnabled && vkCmdEndDebugUtilsLabelEXT)
			vkCmdEndDebugUtilsLabelEXT(commandBuffer);
	}

	void VulkanDevice::BeginProfilingFrame(uint32_t frameIndex)
	{
		GpuFrameDebugInfo& debugInfo = GetMutableGpuFrameDebugInfo();

		if (m_TimestampsSupported && !m_RecordedScopes[frameIndex].empty())
		{
			uint32_t queryCount = m_NextQuerySlot[frameIndex];
			std::vector<uint64_t> timestamps(queryCount);

			// These queries are guaranteed complete: the caller only reaches
			// here after waiting on this exact frame-in-flight slot's fence
			// (VulkanSwapchain::AcquireNextImage), which can't have signalled
			// until all of this slot's previous GPU work, timestamp writes
			// included, finished.
			VkResult result = vkGetQueryPoolResults(m_Device, m_TimestampPools[frameIndex], 0, queryCount,
				timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

			if (result == VK_SUCCESS)
			{
				debugInfo.PassTimings.clear();
				for (const GpuScopeRecord& scope : m_RecordedScopes[frameIndex])
				{
					uint64_t deltaTicks = timestamps[scope.EndQuery] - timestamps[scope.BeginQuery];
					float ms = (float)((double)deltaTicks * m_TimestampPeriodNs / 1000000.0);
					debugInfo.PassTimings.push_back({ scope.Name, ms });
				}

				debugInfo.Available = true;
			}
		}

		debugInfo.FrameInFlightIndex = frameIndex;
		debugInfo.Barriers.clear();

		m_NextQuerySlot[frameIndex] = 0;
		m_RecordedScopes[frameIndex].clear();
		m_ScopeBeginStack[frameIndex].clear();
		m_ScopeNameStack[frameIndex].clear();
	}

	void VulkanDevice::CmdResetTimestampPool(VkCommandBuffer commandBuffer, uint32_t frameIndex)
	{
		if (!m_TimestampsSupported)
			return;

		vkCmdResetQueryPool(commandBuffer, m_TimestampPools[frameIndex], 0, kMaxTimestampsPerFrame);
	}

	namespace {

		const char* VkImageLayoutToString(VkImageLayout layout)
		{
			switch (layout)
			{
				case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
				case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
				case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
				case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
				case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
				case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT_SRC_KHR";
				default: return "OTHER";
			}
		}

	}

	void VulkanDevice::RecordBarrierForDebug(const std::string& resourceName, VkImageLayout oldLayout, VkImageLayout newLayout) const
	{
		GpuBarrierRecord record;
		record.ResourceName = resourceName;
		record.OldLayout = VkImageLayoutToString(oldLayout);
		record.NewLayout = VkImageLayoutToString(newLayout);
		GetMutableGpuFrameDebugInfo().Barriers.push_back(record);
	}

}
