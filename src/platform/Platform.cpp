// The Platform on GLFW. This file and Window.cpp are the only ones that
// include GLFW: nothing outside hearth/platform names it.

// No OpenGL header (GLFW includes one by default), and GLFW's Vulkan
// declarations.
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "hearth/platform/Platform.h"

#include "hearth/Assert.h"
#include "hearth/Log.h"

#include <algorithm>
#include <cctype>

namespace hearth::platform {

	HEARTH_LOG_CATEGORY(LogPlatform)

	namespace {

		void ErrorCallback(int error, const char* description)
		{
			HEARTH_LOG_ERROR(LogPlatform, "GLFW Error ({0}): {1}", error, description);
		}

		void SilentErrorCallback(int, const char*) {}

		int GlfwPlatformOf(Backend backend)
		{
			switch (backend)
			{
			case Backend::Any: return GLFW_ANY_PLATFORM;
			case Backend::X11: return GLFW_PLATFORM_X11;
			case Backend::Wayland: return GLFW_PLATFORM_WAYLAND;
			case Backend::Win32: return GLFW_PLATFORM_WIN32;
			case Backend::Cocoa: return GLFW_PLATFORM_COCOA;
			case Backend::Null: return GLFW_PLATFORM_NULL;
			}
			return GLFW_ANY_PLATFORM;
		}

		Backend BackendOf(int glfwPlatform)
		{
			switch (glfwPlatform)
			{
			case GLFW_PLATFORM_X11: return Backend::X11;
			case GLFW_PLATFORM_WAYLAND: return Backend::Wayland;
			case GLFW_PLATFORM_WIN32: return Backend::Win32;
			case GLFW_PLATFORM_COCOA: return Backend::Cocoa;
			default: return Backend::Null;
			}
		}

	} // namespace

	std::optional<Backend> ParseBackend(std::string_view name)
	{
		std::string lower(name);
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (lower == "x11") return Backend::X11;
		if (lower == "wayland") return Backend::Wayland;
		if (lower == "win32") return Backend::Win32;
		if (lower == "cocoa") return Backend::Cocoa;
		if (lower == "null") return Backend::Null;
		if (lower == "native" || lower == "any") return Backend::Any;
		return std::nullopt;
	}

	const char* BackendName(Backend backend)
	{
		switch (backend)
		{
		case Backend::Any: return "Any";
		case Backend::X11: return "X11";
		case Backend::Wayland: return "Wayland";
		case Backend::Win32: return "Win32";
		case Backend::Cocoa: return "Cocoa";
		case Backend::Null: return "Null";
		}
		return "?";
	}

	std::unique_ptr<Platform> Platform::Create(const PlatformDesc& desc)
	{
		// GLFW is process-wide, so its own state says whether a Platform
		// exists: before glfwInit, glfwGetPlatform reports 0 (and an error,
		// which is silenced here).
		glfwSetErrorCallback(SilentErrorCallback);
		if (glfwGetPlatform() != 0)
		{
			glfwSetErrorCallback(ErrorCallback);
			HEARTH_LOG_ERROR(LogPlatform, "a Platform already exists: there is one window system per process");
			return nullptr;
		}
		glfwSetErrorCallback(ErrorCallback);

		glfwInitHint(GLFW_PLATFORM, GlfwPlatformOf(desc.Requested));
		if (!glfwInit())
		{
			if (desc.Requested == Backend::Any || desc.Requested == Backend::Null)
			{
				HEARTH_LOG_ERROR(LogPlatform, "could not start the window system");
				return nullptr;
			}
			// A requested window system may be missing (X11 with no X server
			// running): the platform's own choice instead of failing.
			HEARTH_LOG_WARN(LogPlatform, "Requested GLFW platform unavailable - retrying with GLFW platform default");
			glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
			if (!glfwInit())
			{
				HEARTH_LOG_ERROR(LogPlatform, "could not start the window system");
				return nullptr;
			}
		}
		return std::unique_ptr<Platform>(new Platform(BackendOf(glfwGetPlatform())));
	}

	Platform::Platform(Backend backend) : m_Backend(backend) {}

	Platform::~Platform()
	{
		HEARTH_ASSERT(m_WindowCount == 0, "the Platform outlived by a Window: destroy every Window first");
		glfwTerminate();
	}

	Caps Platform::GetCaps() const
	{
		Caps caps;
		switch (m_Backend)
		{
		case Backend::X11:
		case Backend::Win32:
			caps.CanTrackGlobalCursor = true;
			caps.CanPlaceWindows = true;
			caps.CanMinimize = true;
			break;
		default:
			break;
		}
		return caps;
	}

	void Platform::Pump()
	{
		glfwPollEvents();
	}

	std::vector<Event> Platform::TakeEvents()
	{
		std::vector<Event> events;
		events.swap(m_Events);
		return events;
	}

	double Platform::Time() const
	{
		return glfwGetTime();
	}

	bool Platform::GetGamepad(int index, GamepadState& out) const
	{
		GLFWgamepadstate state;
		if (index < 0 || GLFW_JOYSTICK_1 + index > GLFW_JOYSTICK_LAST || !glfwGetGamepadState(GLFW_JOYSTICK_1 + index, &state))
			return false;
		for (int i = 0; i < 15; ++i)
			out.Buttons[i] = state.buttons[i] == GLFW_PRESS;
		for (int i = 0; i < 6; ++i)
			out.Axes[i] = state.axes[i];
		return true;
	}

	std::string Platform::GetClipboard() const
	{
		const char* text = glfwGetClipboardString(nullptr);
		return text ? text : "";
	}

	void Platform::SetClipboard(const std::string& text)
	{
		glfwSetClipboardString(nullptr, text.c_str());
	}

	std::vector<const char*> Platform::VulkanInstanceExtensions() const
	{
		uint32_t count = 0;
		const char** names = glfwGetRequiredInstanceExtensions(&count);
		if (!names)
			return {};
		return std::vector<const char*>(names, names + count);
	}

	bool Platform::VulkanPresentationSupport(VkInstance instance, VkPhysicalDevice device, uint32_t queueFamily) const
	{
		return glfwGetPhysicalDevicePresentationSupport(instance, device, queueFamily) == GLFW_TRUE;
	}

} // namespace hearth::platform
