#pragma once

// The OS platform: the window system, the event pump, the clock, gamepads
// and the clipboard. A program has at most one Platform at a time, owned by
// whatever runs its loop; every Window is made from it and must be destroyed
// before it.

#include "hearth/platform/Event.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Vulkan's handle types, declared the way vulkan_core.h declares them, so a
// program that uses the surface helpers needs no Vulkan header from here
// (and one that includes Vulkan's own headers sees the same types).
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
// The same test vulkan_core.h makes (VK_USE_64_BIT_PTR_DEFINES).
#if defined(__LP64__) || defined(_WIN64) || (defined(__x86_64__) && !defined(__ILP32__)) || defined(_M_X64) || defined(__ia64) || defined(_M_IA64) || defined(__aarch64__) || defined(__powerpc64__) || (defined(__riscv) && __riscv_xlen == 64)
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
#else
typedef uint64_t VkSurfaceKHR;
#endif
struct VkAllocationCallbacks;

namespace hearth::platform {

	// The window system the platform runs on. Any lets the platform choose;
	// Null has no windows on screen and no events from the OS (offscreen and
	// headless runs, and tests).
	enum class Backend : uint8_t { Any, X11, Wayland, Win32, Cocoa, Null };

	// "x11", "wayland", "win32", "cocoa", "null", "native"/"any"; nullopt for
	// anything else.
	std::optional<Backend> ParseBackend(std::string_view name);
	const char* BackendName(Backend backend);

	// What the window system lets a program do.
	struct Caps
	{
		bool CanCreateOSWindows = true;
		// Window positions and out-of-window cursor motion during a drag
		// (X11, Win32; not Wayland).
		bool CanTrackGlobalCursor = false;
		// Placing a window (X11, Win32; not Wayland, where the compositor
		// places windows).
		bool CanPlaceWindows = false;
		// Minimizing a window (X11, Win32; Wayland has no request for it).
		bool CanMinimize = false;
	};

	// The standard gamepad mapping: buttons (hearth/platform/Keys.h `gamepad`)
	// and axes (`axis`, -1..1; triggers rest at -1).
	struct GamepadState
	{
		bool Buttons[15] = {};
		float Axes[6] = {};
	};

	struct PlatformDesc
	{
		// The window system to ask for. A request the machine cannot
		// satisfy (X11 with no X server) falls back to the platform's own
		// choice, with a warning through the host's log.
		Backend Requested = Backend::Any;
	};

	class Platform
	{
	public:
		// Null if a Platform already exists (there is one window system per
		// process) or the window system cannot start; the reason goes to the
		// host's log.
		static std::unique_ptr<Platform> Create(const PlatformDesc& desc = {});
		~Platform();
		Platform(const Platform&) = delete;
		Platform& operator=(const Platform&) = delete;

		// The window system it runs on (never Any).
		Backend GetBackend() const { return m_Backend; }
		Caps GetCaps() const;

		// Collects what the OS has for every window (input, focus, size,
		// close requests) into the event queue. The only call that does:
		// whoever owns the Platform calls it once a frame.
		void Pump();
		// The events collected since the last call, in order.
		std::vector<Event> TakeEvents();

		// Seconds since the Platform was created, from a monotonic clock.
		double Time() const;

		// The gamepad at `index` (0 = the first), if one is connected with a
		// standard mapping.
		bool GetGamepad(int index, GamepadState& out) const;

		// The system clipboard's text ("" when it holds none).
		std::string GetClipboard() const;
		void SetClipboard(const std::string& text);

		// The Vulkan instance extensions a surface on this window system
		// needs (empty on Null, or when Vulkan is unavailable).
		std::vector<const char*> VulkanInstanceExtensions() const;
		// Whether a queue family of `device` can present to this window
		// system's surfaces.
		bool VulkanPresentationSupport(VkInstance instance, VkPhysicalDevice device, uint32_t queueFamily) const;

		// The live windows, for a check at shutdown.
		size_t WindowCount() const { return m_WindowCount; }

	private:
		friend class Window;
		friend struct WindowCallbacks;
		explicit Platform(Backend backend);

		Backend m_Backend;
		std::vector<Event> m_Events;
		size_t m_WindowCount = 0;
	};

} // namespace hearth::platform
