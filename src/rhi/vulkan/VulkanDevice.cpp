#include "tepch.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanContext.h" // SetMainContext asserts the context's flag
#include "TinyEngine/Debug/GpuFrameDebugData.h"
#include "TinyEngine/Debug/Profiler.h"
#include "TinyEngine/Core/CrashHandler.h"

#include "loom/loom.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <thread>

namespace TinyEngine {

	namespace {


		// The only validation layer used. If it isn't present on this machine
		// (no Vulkan SDK / LunarG layers installed), startup carries on
		// without it rather than refusing to start, the same way the engine
		// doesn't refuse to start without a debugger attached. The whole
		// debug-utils/validation machinery compiles out of TE_CONF_DIST
		// builds (audit rows A4/A8).
#if !defined(TE_CONF_DIST)
		constexpr const char* g_ValidationLayerName = "VK_LAYER_KHRONOS_validation";

		// Kept separate from the log itself (Log.h has no ring buffer, and
		// giving it one just for this would be a bigger change than the GPU
		// frame debugger's validation panel actually needs) — capped so a
		// noisy run doesn't grow this forever.
		constexpr size_t kMaxBufferedValidationMessages = 50;

		void BufferValidationMessage(GpuFrameDebugSink* sink, VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message)
		{
			if (!sink)
				return;
			const char* severityText = "INFO";
			if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
				severityText = "ERROR";
			else if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
				severityText = "WARN";

			// T-F-C: the messenger fires on whichever thread made the call -
			// the append serializes with the submit thread's frame writes
			// (the sink's lock).
			GpuFrameDebugSink::Writer debug = sink->Write();
			std::vector<std::string>& messages = debug.Info.ValidationMessages;
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

			// F6: log the VUID id alongside the message - the pMessage-only
			// log dropped the VUID name, which let THREADING ERRORs (no VUID
			// token in pMessage) escape the log-grep gates entirely.
			const char* messageId = callbackData->pMessageIdName ? callbackData->pMessageIdName : "<no-id>";
			switch (severity)
			{
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
					TE_CORE_ERROR(LogVulkan, "{0} [{1}]", callbackData->pMessage, messageId);
					break;
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
					TE_CORE_WARN(LogVulkan, "{0} [{1}]", callbackData->pMessage, messageId);
					break;
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
					TE_CORE_INFO(LogVulkan, "{0} [{1}]", callbackData->pMessage, messageId);
					break;
				default:
					TE_CORE_DEBUG(LogVulkan, "{0} [{1}]", callbackData->pMessage, messageId);
					break;
			}

			// The sink travels as the messenger's pUserData (the RhiDevice
			// subsystem's member).
			BufferValidationMessage(static_cast<GpuFrameDebugSink*>(userData), severity, callbackData->pMessage);

			// Returning true here would abort the call that triggered the message,
			// which is almost never what you want outside of a targeted test.
			return VK_FALSE;
		}
#endif // !TE_CONF_DIST

	}

	void VulkanDevice::Init(GLFWwindow* windowForExtensionQuery)
	{
		TE_PROFILE_FUNCTION();

		// The Offscreen mode passes no window
		// - the surface-dependent selections below skip the present
		// requirement and the swapchain extension.
		m_SurfaceEnabled = windowForExtensionQuery != nullptr;
		// The creating thread is the game thread (the first window's
		// VulkanContext::Init); the single-time commands and the upload
		// batch assert it from here on.
		m_GameThreadId = std::this_thread::get_id();

		VK_CHECK(volkInitialize());

		CreateInstance(windowForExtensionQuery);
		volkLoadInstance(m_Instance);

#if !defined(TE_CONF_DIST)
		if (m_ValidationEnabled)
			CreateDebugMessenger();
#endif // !TE_CONF_DIST

		PickPhysicalDevice();
		CreateLogicalDevice();
		volkLoadDevice(m_Device);
		if (m_CalibrationExtension != nullptr)
		{
			m_GetCalibratedTimestamps = (std::strcmp(m_CalibrationExtension, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0)
				? vkGetCalibratedTimestampsKHR : (PFN_vkGetCalibratedTimestampsKHR)vkGetCalibratedTimestampsEXT;
		}

		CreateAllocator();
		CreateCommandPools();
		// The timeline semaphore must exist before the timestamp pools
		// - each pool's one-time reset submits through
		// EndSingleTimeCommands, which now signals + waits on the
		// timeline (the old order handed it a null semaphore).
		CreateTimelineSemaphore();
		CreateTimestampQueryPools();
	}

	void VulkanDevice::CreateTimelineSemaphore()
	{
		VkSemaphoreTypeCreateInfo timelineInfo{};
		timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		timelineInfo.initialValue = 0;

		VkSemaphoreCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		createInfo.pNext = &timelineInfo;

		VK_CHECK(vkCreateSemaphore(m_Device, &createInfo, nullptr, &m_TimelineSemaphore));
		SetObjectName(VK_OBJECT_TYPE_SEMAPHORE, (uint64_t)m_TimelineSemaphore, "Graphics Queue Timeline");
		m_TimelineValue = 0;
	}

	uint64_t VulkanDevice::AllocateTimelineValue()
	{
		return m_TimelineValue.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	void VulkanDevice::AdvanceTimelineValue(uint64_t value)
	{
		// A caller submitting an explicit signal value must not let a later
		// AllocateTimelineValue hand back a value <= one already signalled.
		uint64_t current = m_TimelineValue.load(std::memory_order_relaxed);
		while (current < value && !m_TimelineValue.compare_exchange_weak(current, value, std::memory_order_relaxed))
			;
	}

	void VulkanDevice::WaitForTimelineValue(uint64_t value)
	{
		if (value == 0)
			return; // the timeline starts at 0 - nothing submitted yet
		VkSemaphoreWaitInfo waitInfo{};
		waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
		waitInfo.semaphoreCount = 1;
		waitInfo.pSemaphores = &m_TimelineSemaphore;
		waitInfo.pValues = &value;
		VK_CHECK(vkWaitSemaphores(m_Device, &waitInfo, UINT64_MAX));
	}

	uint64_t VulkanDevice::GetCompletedTimelineValue()
	{
		uint64_t value = 0;
		if (m_TimelineSemaphore != VK_NULL_HANDLE)
			vkGetSemaphoreCounterValue(m_Device, m_TimelineSemaphore, &value);
		return value;
	}

	void VulkanDevice::Shutdown()
	{
		TE_PROFILE_FUNCTION();

		if (m_Device != VK_NULL_HANDLE)
			vkDeviceWaitIdle(m_Device);

		// The device is idle - reclaim any uploaded-batch buffers +
		// retired staging before their pool/allocator die.
		for (auto& [buffer, allocation] : m_UploadBatch.PendingStaging)
			vmaDestroyBuffer(m_Allocator, buffer, allocation);
		m_UploadBatch.PendingStaging.clear();
		for (VkCommandBuffer buffer : m_UploadBatch.SubmittedBuffers)
			vkFreeCommandBuffers(m_Device, m_TransientCommandPool, 1, &buffer);
		m_UploadBatch.SubmittedBuffers.clear();
		m_UploadBatch.SubmittedValue = 0;

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

		if (m_TimelineSemaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(m_Device, m_TimelineSemaphore, nullptr);

		if (m_Allocator != VK_NULL_HANDLE)
			vmaDestroyAllocator(m_Allocator);

		if (m_Device != VK_NULL_HANDLE)
			vkDestroyDevice(m_Device, nullptr);

#if !defined(TE_CONF_DIST)
		if (m_DebugMessenger != VK_NULL_HANDLE)
			vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
#endif // !TE_CONF_DIST

		if (m_Instance != VK_NULL_HANDLE)
			vkDestroyInstance(m_Instance, nullptr);
	}

	bool VulkanDevice::CheckValidationLayerSupport() const
	{
#if !defined(TE_CONF_DIST)
		uint32_t layerCount = 0;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

		std::vector<VkLayerProperties> availableLayers(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

		for (const VkLayerProperties& layer : availableLayers)
		{
			if (strcmp(layer.layerName, g_ValidationLayerName) == 0)
				return true;
		}
#endif // !TE_CONF_DIST

		return false;
	}

	void VulkanDevice::CreateInstance(GLFWwindow* windowForExtensionQuery)
	{
		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "TinyEngine App";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "TinyEngine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		// Vulkan 1.3 core is the baseline.
		// Dynamic rendering and synchronization2 are both used unconditionally as core
		// features rather than extensions further down this backend.
		appInfo.apiVersion = VK_API_VERSION_1_3;

		// The Offscreen mode passes no window - the surface/swapchain
		// extensions are omitted entirely (the device is created
		// display-agnostic; the NVIDIA/Mesa drivers allow this with no
		// X/Wayland present).
		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions = nullptr;
		std::vector<const char*> extensions;
		if (windowForExtensionQuery != nullptr)
		{
			glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
			TE_CORE_ASSERT(glfwExtensions, "GLFW could not provide the instance extensions Vulkan surface creation needs on this platform!");
			extensions.assign(glfwExtensions, glfwExtensions + glfwExtensionCount);
		}

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

		// Chaining a messenger create info onto the instance create info means
		// validation messages raised by vkCreateInstance/vkDestroyInstance
		// themselves are also caught, which the messenger object created
		// afterwards would otherwise miss.
#if !defined(TE_CONF_DIST)
		VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo{};
		if (m_ValidationEnabled)
		{
			const char* layers[] = { g_ValidationLayerName };
			createInfo.enabledLayerCount = 1;
			createInfo.ppEnabledLayerNames = layers;

			// Verbose/info would also surface the loader's own manifest discovery
			// chatter on every single run (which layers/drivers it found and where),
			// not just messages from the validation layer itself. Only warnings
			// and errors are surfaced.
			messengerCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			messengerCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messengerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
				| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messengerCreateInfo.pfnUserCallback = DebugMessengerCallback;
			messengerCreateInfo.pUserData = m_GpuFrameDebugSink;

			createInfo.pNext = &messengerCreateInfo;
		}
#endif // !TE_CONF_DIST

		VK_CHECK(vkCreateInstance(&createInfo, nullptr, &m_Instance));

		TE_CORE_INFO(LogVulkan, "Vulkan Instance Created (validation {0})", m_ValidationEnabled ? "enabled" : "disabled");
	}

#if !defined(TE_CONF_DIST)
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
		createInfo.pUserData = m_GpuFrameDebugSink;

		VK_CHECK(vkCreateDebugUtilsMessengerEXT(m_Instance, &createInfo, nullptr, &m_DebugMessenger));
	}
#endif // !TE_CONF_DIST

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

			// Vulkan 1.3 core is a hard requirement, not something to fall back
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
				// The Offscreen mode has no surface - the present
				// capability is not required (any graphics-capable queue
				// family works).
				const bool canPresent = !m_SurfaceEnabled
					|| glfwGetPhysicalDevicePresentationSupport(m_Instance, device, i) == GLFW_TRUE;

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
		// The capture sidecar's GPU identity (the CI golden-source
		// policy's key).
		m_DeviceName = properties.deviceName;
		m_DriverVersion = properties.driverVersion;
		m_ApiVersion = properties.apiVersion;
		// The GPU in every crash report.
		char driver[16];
		std::snprintf(driver, sizeof(driver), "0x%08x", properties.driverVersion);
		CrashHandler::SetGpu(properties.deviceName, driver);

		// Needed to convert GPU timestamp query results (raw ticks) into
		// milliseconds for the GPU frame debugger. timestampValidBits == 0 on
		// the chosen queue family would mean timestamps aren't supported there
		// at all, rare but not impossible, checked rather than assumed the
		// way samplerAnisotropy is.
		m_TimestampPeriodNs = properties.limits.timestampPeriod;

		// P-C: the mesh/debug push blocks reach 132 bytes; the spec minimum
		// is 128, so the RHI checks this at Init rather than discovering it
		// as a vkCreatePipelineLayout validation error.
		m_MaxPushConstantsSize = properties.limits.maxPushConstantsSize;

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());
		m_TimestampsSupported = queueFamilies[m_GraphicsQueueFamily].timestampValidBits > 0;
		const uint32_t validBits = queueFamilies[m_GraphicsQueueFamily].timestampValidBits;
		m_TimestampMask = (validBits == 0 || validBits >= 64) ? ~0ull : ((1ull << validBits) - 1ull);
		DetectCalibratedTimestamps();
	}

	void VulkanDevice::DetectCalibratedTimestamps()
	{
#if !defined(TE_CONF_DIST)
		if (!m_TimestampsSupported)
			return;
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> available(count);
		vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &count, available.data());
		auto has = [&](const char* name) {
			return std::any_of(available.begin(), available.end(),
				[&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
		};
		PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR getDomains = nullptr;
		if (has(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) && vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)
		{
			m_CalibrationExtension = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
			getDomains = vkGetPhysicalDeviceCalibrateableTimeDomainsKHR;
		}
		else if (has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) && vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)
		{
			m_CalibrationExtension = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
			getDomains = vkGetPhysicalDeviceCalibrateableTimeDomainsEXT;
		}
		if (getDomains == nullptr)
		{
			TE_CORE_INFO(LogVulkan, "Profiler: calibrated timestamps unsupported; the GPU lane is anchored (uncalibrated).");
			return;
		}
		uint32_t domainCount = 0;
		getDomains(m_PhysicalDevice, &domainCount, nullptr);
		std::vector<VkTimeDomainKHR> domains(domainCount);
		getDomains(m_PhysicalDevice, &domainCount, domains.data());
		// The profiler's clock is std::chrono::steady_clock: CLOCK_MONOTONIC
		// on Linux (libstdc++/libc++), QueryPerformanceCounter on Windows.
#if defined(_WIN32)
		const VkTimeDomainKHR host = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
		const VkTimeDomainKHR host = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
		const bool hasDevice = std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_DEVICE_KHR) != domains.end();
		const bool hasHost = std::find(domains.begin(), domains.end(), host) != domains.end();
		if (!hasDevice || !hasHost)
		{
			TE_CORE_INFO(LogVulkan, "Profiler: {} lacks the device + steady-clock domains; the GPU lane is anchored (uncalibrated).", m_CalibrationExtension);
			m_CalibrationExtension = nullptr;
			return;
		}
		m_HostTimeDomain = host;
#endif // !TE_CONF_DIST
	}

	void VulkanDevice::CalibrateGpuClock()
	{
#if !defined(TE_CONF_DIST)
		VkCalibratedTimestampInfoKHR infos[2]{};
		infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
		infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
		infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
		infos[1].timeDomain = m_HostTimeDomain;
		uint64_t values[2] = {};
		uint64_t maxDeviation = 0;
		if (m_GetCalibratedTimestamps(m_Device, 2, infos, values, &maxDeviation) != VK_SUCCESS)
			return;
		// A preempted sample (a wide bracket) is noise: keep the previous one.
		if (m_GpuClockValid && maxDeviation > 10000)
			return;

		uint64_t hostNs = values[1];
#if defined(_WIN32)
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		const uint64_t f = (uint64_t)frequency.QuadPart;
		hostNs = (values[1] / f) * 1000000000ull + (values[1] % f) * 1000000000ull / f;
#endif
		GpuClockMapping next;
		next.DeviceTick = values[0] & m_TimestampMask;
		next.CpuNs = hostNs;
		next.PeriodNs = (double)m_TimestampPeriodNs;
		next.TickMask = m_TimestampMask;
		double errorNs = 0.0;
		if (m_GpuClockValid)
		{
			// The device clock runs a few ppm off its nominal period against
			// the steady clock (6.6 ppm measured on the RTX 3080 Ti, 2 ms
			// over 5 minutes). The rate is learned from the sample pairs
			// (smoothed: each sample carries ~2 us of bracket jitter), so a
			// pass between samples is placed by the measured rate.
			const uint64_t ticks = (next.DeviceTick - m_GpuClock.DeviceTick) & m_TimestampMask;
			const double cpuNs = (double)next.CpuNs - (double)m_GpuClock.CpuNs;
			if (ticks > 0 && cpuNs > 2.0e8)
			{
				const double measured = cpuNs / (double)ticks;
				m_GpuPeriodEstimate = (m_GpuPeriodEstimate > 0.0) ? 0.9 * m_GpuPeriodEstimate + 0.1 * measured : measured;
			}
			// The residual: where the previous mapping put this instant.
			errorNs = (double)next.CpuNs - (double)m_GpuClock.ToCpuNs(next.DeviceTick);
		}
		else
		{
			TE_CORE_INFO(LogVulkan, "Profiler: GPU timestamps calibrated ({}, max deviation {} ns).", m_CalibrationExtension, maxDeviation);
		}
		if (m_GpuPeriodEstimate > 0.0)
			next.PeriodNs = m_GpuPeriodEstimate;
		m_GpuClock = next;
		m_GpuClockValid = true;
		TinyEngine::Profiler::Get().PlotValue("GPU clock error (us)", errorNs / 1000.0);
		TinyEngine::Profiler::Get().PlotValue("GPU clock rate (ppm)",
			(m_GpuPeriodEstimate > 0.0 ? m_GpuPeriodEstimate / (double)m_TimestampPeriodNs - 1.0 : 0.0) * 1.0e6);
#endif // !TE_CONF_DIST
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
		// Descriptor indexing and timeline semaphores (1.2 core) are also
		// enabled, for Renderer2D's bindless texture array.
		VkPhysicalDeviceVulkan13Features vulkan13Features{};
		vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13Features.dynamicRendering = VK_TRUE;
		vulkan13Features.synchronization2 = VK_TRUE;
		// The gizmo overlay fragment shader uses `discard` alongside fwidth()
		// derivatives; glslang emits OpDemoteToHelperInvocation for that
		// combination (demote keeps helper invocations alive so the
		// derivative is still correct), which needs this feature enabled.
		// Core in Vulkan 1.3 (promoted from VK_EXT_shader_demote_to_helper_invocation),
		// universally supported on 1.3-capable hardware.
		vulkan13Features.shaderDemoteToHelperInvocation = VK_TRUE;

		VkPhysicalDeviceVulkan12Features vulkan12Features{};
		vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		vulkan12Features.descriptorIndexing = VK_TRUE;
		vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
		vulkan12Features.descriptorBindingVariableDescriptorCount = VK_TRUE;
		vulkan12Features.runtimeDescriptorArray = VK_TRUE;
		vulkan12Features.timelineSemaphore = VK_TRUE;
		// The global bindless set writes descriptors after binding
		// (UPDATE_AFTER_BIND) - the three per-type feature bits the
		// descriptorIndexing umbrella does NOT enable. The
		// update-unused-while-pending bit is a safety net, not the reuse
		// mechanism (the retire queue is).
		vulkan12Features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
		vulkan12Features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
		vulkan12Features.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
		vulkan12Features.pNext = &vulkan13Features;

		// Anisotropic filtering is an optional feature (unlike GL, where a max
		// anisotropy limit is just always queryable), so it has to actually be
		// checked and requested rather than assumed on, or every sampler
		// created with anisotropyEnable = VK_TRUE is a validation error.
		// Non-solid polygon modes (the wireframe debug view's LINE pipeline)
		// are the same shape: checked + requested here, or requesting
		// VK_POLYGON_MODE_LINE is VUID-...-polygonMode-01507.
		VkPhysicalDeviceFeatures supportedFeatures;
		vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supportedFeatures);
		m_SamplerAnisotropySupported = supportedFeatures.samplerAnisotropy == VK_TRUE;
		m_FillModeNonSolidSupported = supportedFeatures.fillModeNonSolid == VK_TRUE;

		VkPhysicalDeviceFeatures2 features2{};
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features2.pNext = &vulkan12Features;
		features2.features.samplerAnisotropy = m_SamplerAnisotropySupported ? VK_TRUE : VK_FALSE;
		features2.features.fillModeNonSolid = m_FillModeNonSolidSupported ? VK_TRUE : VK_FALSE;

		// The Offscreen mode omits the swapchain extension (no
		// surface).
		std::vector<const char*> deviceExtensions;
		if (m_SurfaceEnabled)
			deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (m_CalibrationExtension != nullptr)
			deviceExtensions.push_back(m_CalibrationExtension);
		VkDeviceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.pNext = &features2;
		createInfo.queueCreateInfoCount = 1;
		createInfo.pQueueCreateInfos = &queueCreateInfo;
		createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
		createInfo.ppEnabledExtensionNames = deviceExtensions.empty() ? nullptr : deviceExtensions.data();
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
		// P-A2: pools are keyed by (frame slot, record thread). T-F-B.3: the
		// count covers the main thread + every Loom pool worker 1:1 (loom
		// assigns 0 to the main thread and 1..workerCount to the workers;
		// the clamped RecordThreadIndex is the defensive fallback for any
		// helping thread beyond the range - none exist in the tree today,
		// and lanes never steal).
		uint32_t hardwareThreads = std::thread::hardware_concurrency() == 0
			? 1u
			: (uint32_t)std::thread::hardware_concurrency();
		m_RecordThreadCount = std::min(MaxRecordThreads * 4u, std::max(MaxRecordThreads, hardwareThreads));
		m_CommandPools.resize(TotalFrameSlots * m_RecordThreadCount, VK_NULL_HANDLE);

		VkCommandPoolCreateInfo poolCreateInfo{};
		poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		// Individually resettable so each frame's pool can be reset in one call once
		// that frame's timeline value has passed, rather than freeing/reallocating buffers.
		poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolCreateInfo.queueFamilyIndex = m_GraphicsQueueFamily;

		for (uint32_t frame = 0; frame < TotalFrameSlots; frame++)
		{
			for (uint32_t thread = 0; thread < m_RecordThreadCount; thread++)
			{
				VK_CHECK(vkCreateCommandPool(m_Device, &poolCreateInfo, nullptr, &m_CommandPools[frame * m_RecordThreadCount + thread]));
				SetObjectName(VK_OBJECT_TYPE_COMMAND_POOL,
					(uint64_t)m_CommandPools[frame * m_RecordThreadCount + thread],
					"Command Pool (frame " + std::to_string(frame) + ", thread " + std::to_string(thread) + ")");
			}
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
		m_TimestampPools.resize(TotalFrameSlots, VK_NULL_HANDLE);
		// T-F-B.3: the counters are per frame slot (atomic - the parallel
		// record workers allocate slots concurrently; the fetch_add keeps
		// the used prefix contiguous so the readback stays one range); the
		// scope stacks + recorded scopes are per (frame slot, record thread).
		m_NextQuerySlot = std::make_unique<std::atomic<uint32_t>[]>(TotalFrameSlots);
		for (uint32_t i = 0; i < TotalFrameSlots; i++)
			m_NextQuerySlot[i].store(0, std::memory_order_relaxed);
		m_ScopeBeginStack.resize((size_t)TotalFrameSlots * m_RecordThreadCount);
		m_ScopeNameStack.resize((size_t)TotalFrameSlots * m_RecordThreadCount);
		m_RecordedScopes.resize((size_t)TotalFrameSlots * m_RecordThreadCount);

		if (!m_TimestampsSupported)
		{
			TE_CORE_WARN(LogVulkan, "This GPU's graphics queue doesn't report timestampValidBits, the GPU frame debugger's pass timings will stay unavailable.");
			return;
		}

		VkQueryPoolCreateInfo poolCreateInfo{};
		poolCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		poolCreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
		poolCreateInfo.queryCount = kMaxTimestampsPerFrame;

		for (uint32_t i = 0; i < TotalFrameSlots; i++)
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

	uint32_t VulkanDevice::AllocateWindowSlot(VulkanContext* owner)
	{
		uint32_t slot = 0;
		while (slot < MaxWindowSlots && m_WindowSlotClaimed[slot])
			slot++;
		TE_CORE_ASSERT(slot < MaxWindowSlots, "More live windows than VulkanDevice::MaxWindowSlots allows!");
		m_WindowSlotClaimed[slot] = true;
		m_WindowContexts[slot] = owner;
		return slot * FramesInFlight;
	}

	void VulkanDevice::ReleaseWindowSlot(uint32_t slotBase)
	{
		const uint32_t slot = slotBase / FramesInFlight;
		TE_CORE_ASSERT(slot < MaxWindowSlots, "ReleaseWindowSlot with an out-of-range slot base");
		m_WindowContexts[slot] = nullptr;
	}

	void VulkanDevice::RecycleWindowSlot(uint32_t slotBase)
	{
		const uint32_t slot = slotBase / FramesInFlight;
		TE_CORE_ASSERT(slot < MaxWindowSlots, "RecycleWindowSlot with an out-of-range slot base");
		TE_CORE_ASSERT(m_WindowContexts[slot] == nullptr, "RecycleWindowSlot before ReleaseWindowSlot");
		m_WindowSlotClaimed[slot] = false;
	}

	void VulkanDevice::SetMainContext(VulkanContext* context)
	{
		// Set-once - a second set (e.g. a secondary window claiming the main
		// context, or a main window created twice) is a defect, not a silent
		// overwrite. The context must be the one created with
		// WindowProps::IsMainWindow (not "owns window slot 0") and live in
		// the window-slot table (it allocated its slot first).
		TE_CORE_ASSERT(m_MainContext == nullptr, "SetMainContext called twice - the main context is set exactly once");
		TE_CORE_ASSERT(context != nullptr && context->IsMainWindow(), "SetMainContext from a context not created as the main window (WindowProps::IsMainWindow)");
		TE_CORE_ASSERT(std::find(m_WindowContexts.begin(), m_WindowContexts.end(), context) != m_WindowContexts.end(),
			"SetMainContext from a context that holds no live window slot (it must allocate its slot first)");
		m_MainContext = context;
	}

	void VulkanDevice::ClearMainContext(VulkanContext* context)
	{
		TE_CORE_ASSERT(m_MainContext == context, "ClearMainContext from a context that is not the main context");
		m_MainContext = nullptr;
	}

	VulkanContext& VulkanDevice::GetMainContext() const
	{
		TE_CORE_ASSERT(m_MainContext != nullptr, "GetMainContext() with no main context - the main window's context must exist in windowed flavors");
		return *m_MainContext;
	}

	void VulkanDevice::AssertGameThread(const char* entryPoint) const
	{
		(void)entryPoint;
		TE_CORE_ASSERT(IsGameThread(), std::string("VulkanDevice::") + entryPoint + " off the game thread - the transient pool and the upload batch are game-thread-only");
	}

	VkCommandBuffer VulkanDevice::BeginSingleTimeCommands()
	{
		AssertGameThread("BeginSingleTimeCommands");
		// Inside an active upload batch every upload records into the
		// batch's one buffer (the batch owns begin/end/submit).
		if (m_UploadBatch.Buffer != VK_NULL_HANDLE)
		{
			m_UploadBatch.Dirty = true;
			return m_UploadBatch.Buffer;
		}

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

	void VulkanDevice::EndSingleTimeCommands(VkCommandBuffer commandBuffer)
	{
		AssertGameThread("EndSingleTimeCommands");
		// The batch owns the end + submit for its own buffer.
		if (m_UploadBatch.Buffer != VK_NULL_HANDLE)
		{
			TE_CORE_ASSERT(commandBuffer == m_UploadBatch.Buffer, "an upload outside the batch ended inside it");
			return;
		}

		VK_CHECK(vkEndCommandBuffer(commandBuffer));

		uint64_t signalValue = 0;
		VkTimelineSemaphoreSubmitInfo timelineInfo{};
		timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timelineInfo.signalSemaphoreValueCount = 1;
		timelineInfo.pSignalSemaphoreValues = &signalValue;

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.pNext = &timelineInfo;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &m_TimelineSemaphore;

		// T-F-C: the queue is externally synchronized - the game-thread
		// uploads and the submit thread's frame submits serialize on it.
		// The timeline VALUE is allocated inside the lock (the same
		// VUID-VkSubmitInfo-pSignalSemaphores-03242 ordering the batch
		// honours).
		{
			std::lock_guard lock(m_QueueMutex);
			signalValue = AllocateTimelineValue();
			VK_CHECK(vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
		}

		// Wait for THIS submission only (not the whole queue): a one-off
		// upload no longer drains every in-flight frame, and any frame
		// submitted after it is not stalled. The staging is reclaimed by
		// the caller once this returns.
		WaitForTimelineValue(signalValue);

		vkFreeCommandBuffers(m_Device, m_TransientCommandPool, 1, &commandBuffer);
	}

	void VulkanDevice::BeginUploadBatch()
	{
		AssertGameThread("BeginUploadBatch");
		TE_CORE_ASSERT(m_UploadBatch.Buffer == VK_NULL_HANDLE, "an upload batch is already active");

		// Reclaim the previous batch's submitted buffers + retired staging
		// once the GPU has passed its submission (conservative: everything
		// is freed at the LATEST submitted value).
		if (m_UploadBatch.SubmittedValue != 0 && GetCompletedTimelineValue() >= m_UploadBatch.SubmittedValue)
		{
			for (auto& [buffer, allocation] : m_UploadBatch.PendingStaging)
				vmaDestroyBuffer(m_Allocator, buffer, allocation);
			m_UploadBatch.PendingStaging.clear();
			for (VkCommandBuffer buffer : m_UploadBatch.SubmittedBuffers)
				vkFreeCommandBuffers(m_Device, m_TransientCommandPool, 1, &buffer);
			m_UploadBatch.SubmittedBuffers.clear();
			m_UploadBatch.SubmittedValue = 0;
		}

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = m_TransientCommandPool;
		allocInfo.commandBufferCount = 1;

		VK_CHECK(vkAllocateCommandBuffers(m_Device, &allocInfo, &m_UploadBatch.Buffer));

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(m_UploadBatch.Buffer, &beginInfo));
		m_UploadBatch.Dirty = false;
	}

	void VulkanDevice::EndUploadBatch()
	{
		AssertGameThread("EndUploadBatch");
		VkCommandBuffer commandBuffer = m_UploadBatch.Buffer;
		if (commandBuffer == VK_NULL_HANDLE)
			return; // no batch active

		// A batch with nothing recorded submits nothing (a recording-state
		// free is legal).
		if (!m_UploadBatch.Dirty)
		{
			m_UploadBatch.Buffer = VK_NULL_HANDLE;
			vkFreeCommandBuffers(m_Device, m_TransientCommandPool, 1, &commandBuffer);
			return;
		}

		VK_CHECK(vkEndCommandBuffer(commandBuffer));

		// The batch signals the graphics timeline at a fresh value: its
		// command buffer + retired staging are reclaimed once the GPU
		// passes it (BeginUploadBatch's drain), which also makes the
		// retire-queue stamps stay conservative (the batch consumes the
		// "next submit value" just like a frame submit would).
		// The value is allocated INSIDE the queue lock alongside the
		// submit (see VulkanSwapchain::SubmitAndPresent) - concurrent
		// allocators (two windows' frame submits) must not execute out of
		// allocation order (VUID-VkSubmitInfo-pSignalSemaphores-03242).
		uint64_t signalValue = 0;

		VkTimelineSemaphoreSubmitInfo timelineInfo{};
		timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timelineInfo.signalSemaphoreValueCount = 1;
		timelineInfo.pSignalSemaphoreValues = &signalValue;

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.pNext = &timelineInfo;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &m_TimelineSemaphore;

		// No waitIdle: the batch submits and moves on. Queue submission order
		// plus the frame's acquire timeline wait keep the frame that samples
		// these uploads ordered after them (the one-graphics-timeline
		// invariant).
		{
			std::lock_guard lock(m_QueueMutex);
			signalValue = AllocateTimelineValue();
			VK_CHECK(vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
		}

		m_UploadBatch.SubmittedBuffers.push_back(commandBuffer);
		m_UploadBatch.SubmittedValue = signalValue;
		m_UploadBatch.Buffer = VK_NULL_HANDLE;
		m_UploadBatch.Dirty = false;
	}

	bool VulkanDevice::BatchRetireStaging(VkBuffer buffer, VmaAllocation allocation)
	{
		AssertGameThread("BatchRetireStaging");
		if (m_UploadBatch.Buffer == VK_NULL_HANDLE)
			return false;
		m_UploadBatch.PendingStaging.emplace_back(buffer, allocation);
		return true;
	}

	void VulkanDevice::SetObjectName(VkObjectType type, uint64_t handle, const std::string& name) const
	{
#if !defined(TE_CONF_DIST)
		if (!m_ValidationEnabled || !vkSetDebugUtilsObjectNameEXT)
			return;

		VkDebugUtilsObjectNameInfoEXT nameInfo{};
		nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		nameInfo.objectType = type;
		nameInfo.objectHandle = handle;
		nameInfo.pObjectName = name.c_str();

		vkSetDebugUtilsObjectNameEXT(m_Device, &nameInfo);
#endif // !TE_CONF_DIST
		(void)type; (void)handle; (void)name;
	}

	void VulkanDevice::CmdBeginDebugLabel(VkCommandBuffer commandBuffer, uint32_t frameSlot, const std::string& name, const float colour[4], bool timed)
	{
#if !defined(TE_CONF_DIST)
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
			TE_CORE_ASSERT(frameSlot < TotalFrameSlots, "CmdBeginDebugLabel frame index out of range!");

			// T-F-B.3: the slot counter is per frame + atomic (the parallel
			// record workers allocate concurrently); fetch_add keeps the used
			// prefix contiguous for the single-range readback. Running out of
			// slots is a counted, recoverable condition - the label still
			// records, only its timing is skipped (the sentinel on the scope
			// stack). The old assert crashed the dev editor on any frame with
			// ~500+ labelled scopes (Wisp labels every draw batch - the
			// profiler panel at deep zoom), and builds without asserts wrote
			// past the query pool. An untimed label (F1) takes the same
			// sentinel, so the stack stays paired and its end writes no
			// timestamp either.
			uint32_t slot = kNoTimestampSlot;
			if (timed)
			{
				slot = m_NextQuerySlot[frameSlot].fetch_add(1, std::memory_order_relaxed);
				if (slot < kMaxTimestampsPerFrame)
					vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_TimestampPools[frameSlot], slot);
				else
				{
					NoteTimestampOverflow();
					slot = kNoTimestampSlot;
				}
			}

			// The begin/end pairing is always recorded by one worker, so the
			// scope stacks are per (frame, record thread).
			size_t threadKey = (size_t)frameSlot * m_RecordThreadCount + RecordThreadIndex();
			m_ScopeBeginStack[threadKey].push_back(slot);
			m_ScopeNameStack[threadKey].push_back(name);
		}
#endif // !TE_CONF_DIST
		(void)commandBuffer; (void)frameSlot; (void)name; (void)colour; (void)timed;
	}

	void VulkanDevice::CmdEndDebugLabel(VkCommandBuffer commandBuffer, uint32_t frameSlot)
	{
#if !defined(TE_CONF_DIST)
		if (m_TimestampsSupported)
		{
			TE_CORE_ASSERT(frameSlot < TotalFrameSlots, "CmdEndDebugLabel frame index out of range!");

			size_t threadKey = (size_t)frameSlot * m_RecordThreadCount + RecordThreadIndex();

			// Begin/EndDebugLabel calls are always properly paired at every
			// call site in this codebase, but guard anyway rather than pop an
			// empty stack if that's ever not true.
			if (!m_ScopeBeginStack[threadKey].empty())
			{
				uint32_t beginSlot = m_ScopeBeginStack[threadKey].back();
				m_ScopeBeginStack[threadKey].pop_back();
				std::string scopeName = m_ScopeNameStack[threadKey].back();
				m_ScopeNameStack[threadKey].pop_back();

				// A10: an untimed begin stays untimed; an end that finds no
				// slot drops the scope's timing (never a write past the pool).
				uint32_t endSlot = kNoTimestampSlot;
				if (beginSlot != kNoTimestampSlot)
				{
					endSlot = m_NextQuerySlot[frameSlot].fetch_add(1, std::memory_order_relaxed);
					if (endSlot < kMaxTimestampsPerFrame)
						vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, m_TimestampPools[frameSlot], endSlot);
					else
					{
						NoteTimestampOverflow();
						endSlot = kNoTimestampSlot;
					}
				}

				if (endSlot != kNoTimestampSlot)
				{
#if !defined(TE_CONF_DIST)
					uint32_t profilerFrame = TinyEngine::Profiler::Get().CurrentFrameIndex();
#else
					uint32_t profilerFrame = 0;
#endif // !TE_CONF_DIST
					m_RecordedScopes[threadKey].push_back({ scopeName, beginSlot, endSlot, profilerFrame });
				}
			}
		}

		if (m_ValidationEnabled && vkCmdEndDebugUtilsLabelEXT)
			vkCmdEndDebugUtilsLabelEXT(commandBuffer);
#endif // !TE_CONF_DIST
		(void)commandBuffer; (void)frameSlot;
	}

	void VulkanDevice::BeginProfilingFrame(uint32_t frameIndex, bool isMainWindow)
	{
		TE_CORE_ASSERT(frameIndex < TotalFrameSlots, "BeginProfilingFrame frame index out of range!");
#if !defined(TE_CONF_DIST)
		GpuFrameDebugSink::Writer debug = GetGpuFrameDebugSink().Write();
		GpuFrameDebugInfo& debugInfo = debug.Info;
#endif // !TE_CONF_DIST

		// The counter keeps counting past the pool on an overflow frame (A10).
		uint32_t queryCount = std::min(m_NextQuerySlot[frameIndex].load(std::memory_order_relaxed), kMaxTimestampsPerFrame);

		if (isMainWindow && m_TimestampsSupported && queryCount > 0)
		{
			std::vector<uint64_t> timestamps(queryCount);

			// These queries are guaranteed complete: the caller only reaches
			// here after waiting on this exact frame-in-flight slot's fence
			// (VulkanSwapchain::AcquireNextImage), which can't have signalled
			// until all of this slot's previous GPU work, timestamp writes
			// included, finished. The atomic fetch_add allocation keeps the
			// used slots a contiguous [0, queryCount) prefix.
			VkResult result = vkGetQueryPoolResults(m_Device, m_TimestampPools[frameIndex], 0, queryCount,
				timestamps.size() * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

			if (result == VK_SUCCESS)
			{
#if !defined(TE_CONF_DIST)
				if (m_GetCalibratedTimestamps != nullptr && m_CalibrationCountdown-- == 0)
				{
					CalibrateGpuClock();
					m_CalibrationCountdown = kCalibrationInterval - 1;
				}
				debugInfo.PassTimings.clear();
				// The profiler's GPU lane: the tick pairs + the
				// originating frame, in the device-ns domain (the viewer
				// anchors on the frame; the tooltip converts via the
				// period). T-F-B.3: the scopes merge per-thread
				// (thread-major, then recording order) - still fully
				// deterministic.
				for (uint32_t thread = 0; thread < m_RecordThreadCount; thread++)
				{
					for (const GpuScopeRecord& scope : m_RecordedScopes[(size_t)frameIndex * m_RecordThreadCount + thread])
					{
						uint64_t deltaTicks = timestamps[scope.EndQuery] - timestamps[scope.BeginQuery];
						float ms = (float)((double)deltaTicks * m_TimestampPeriodNs / 1000000.0);
						debugInfo.PassTimings.push_back({ scope.Name, ms });

#if !defined(TE_CONF_DIST)
						if (m_GpuClockValid)
							TinyEngine::Profiler::Get().GpuPassPair(0,
								m_GpuClock.ToCpuNs(timestamps[scope.BeginQuery] & m_TimestampMask),
								m_GpuClock.ToCpuNs(timestamps[scope.EndQuery] & m_TimestampMask),
								scope.ProfilerFrameIndex, scope.Name.c_str(), true);
						else
							TinyEngine::Profiler::Get().GpuPassPair(0,
								(uint64_t)((double)timestamps[scope.BeginQuery] * m_TimestampPeriodNs),
								(uint64_t)((double)timestamps[scope.EndQuery] * m_TimestampPeriodNs),
								scope.ProfilerFrameIndex, scope.Name.c_str());
#endif // !TE_CONF_DIST
					}
				}

				debugInfo.Available = true;
#endif // !TE_CONF_DIST
			}
		}

#if !defined(TE_CONF_DIST)
		if (isMainWindow)
		{
			// The panel's "In-Flight Index" is the window's frame-in-flight
			// index, not the full slot (they were equal only while the main
			// window held slot base 0).
			debugInfo.FrameInFlightIndex = frameIndex % FramesInFlight;
			debugInfo.Barriers.clear();

			// Fold the live VMA heap budgets into the debugger snapshot
			// (the caller holds the fence, so the allocator's counters are
			// quiesced for this frame slot).
			// vmaGetHeapBudgets writes the device's heaps only; the rest of
			// the array was uninitialised stack the panels showed as
			// 133507876 MiB heaps.
			VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
			vmaGetHeapBudgets(m_Allocator, budgets);
			const VkPhysicalDeviceMemoryProperties* memoryProperties = nullptr;
			vmaGetMemoryProperties(m_Allocator, &memoryProperties);
			const uint32_t heapCount = memoryProperties ? std::min<uint32_t>(memoryProperties->memoryHeapCount, VK_MAX_MEMORY_HEAPS) : 0;
			debugInfo.HeapBudgetCount = 0;
			for (uint32_t heap = 0; heap < heapCount; heap++)
			{
				if (budgets[heap].budget == 0 && budgets[heap].usage == 0)
					continue;
				GpuHeapBudget& out = debugInfo.HeapBudgets[debugInfo.HeapBudgetCount++];
				out.BytesUsed = budgets[heap].usage;
				out.BytesBudget = budgets[heap].budget;
			}
		}
#endif // !TE_CONF_DIST

		m_NextQuerySlot[frameIndex].store(0, std::memory_order_relaxed);
		for (uint32_t thread = 0; thread < m_RecordThreadCount; thread++)
		{
			size_t threadKey = (size_t)frameIndex * m_RecordThreadCount + thread;
			m_RecordedScopes[threadKey].clear();
			m_ScopeBeginStack[threadKey].clear();
			m_ScopeNameStack[threadKey].clear();
		}
	}

	uint32_t VulkanDevice::RecordThreadIndex() const
	{
		int index = loom::CurrentThreadIndex();
		if (index < 0)
			index = 0;
		// The dev assert catches the pool-count mismatch the silent clamp
		// used to hide (two threads sharing one pool); the clamp stays
		// the release-only fallback.
		TE_CORE_ASSERT((uint32_t)index < m_RecordThreadCount,
			"the loom thread index exceeds the record pool count");
		return (uint32_t)index < m_RecordThreadCount ? (uint32_t)index : m_RecordThreadCount - 1;
	}

#if !defined(TE_CONF_DIST)
	void VulkanDevice::NoteTimestampOverflow()
	{
		if (!m_TimestampOverflowWarned.exchange(true, std::memory_order_relaxed))
			TE_CORE_WARN(LogVulkan, "GPU timestamps: a frame recorded more than {0} timed scopes - the extra scopes keep their labels but go untimed (warned once).",
				kMaxTimestampsPerFrame / 2);
	}
#endif // !TE_CONF_DIST

	void VulkanDevice::CmdResetTimestampPool(VkCommandBuffer commandBuffer, uint32_t frameIndex)
	{
		if (!m_TimestampsSupported)
			return;

		vkCmdResetQueryPool(commandBuffer, m_TimestampPools[frameIndex], 0, kMaxTimestampsPerFrame);
	}

}
