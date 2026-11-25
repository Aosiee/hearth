#pragma once

// An OS window, made from the Platform. Its events (input, focus, size,
// close requests) arrive through Platform::Pump and Platform::TakeEvents,
// each naming this window as its Source.

#include "hearth/platform/Platform.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace hearth::platform {

	struct WindowDesc
	{
		std::string Title = "Hearth";
		uint32_t Width = 1280;
		uint32_t Height = 720;
		// A hidden window exists (it can render and present) but is never
		// shown.
		bool Visible = true;
		uint32_t MinWidth = 0;
		uint32_t MinHeight = 0;
		// Centred on the primary monitor's work area, where the window
		// system lets a program place windows.
		bool Center = false;
	};

	enum class CursorShape : uint8_t { Arrow, IBeam, Crosshair, Hand, ResizeEW, ResizeNS };
	// Normal: shown and free. Hidden: invisible over the window. Disabled:
	// hidden and locked to the window, reporting unbounded motion (a
	// mouse-look camera).
	enum class CursorMode : uint8_t { Normal, Hidden, Disabled };

	class Window
	{
	public:
		Window(Platform& platform, const WindowDesc& desc);
		~Window();
		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		Platform& GetPlatform() const { return m_Platform; }

		// The size in screen coordinates, and of its framebuffer in pixels
		// (they differ on a scaled display).
		std::pair<int, int> Size() const;
		void SetSize(int width, int height);
		std::pair<int, int> FramebufferSize() const;
		std::pair<float, float> ContentScale() const;

		// The position of the window's content area, where the window system
		// reports one (Caps::CanTrackGlobalCursor); nullopt elsewhere.
		std::optional<std::pair<int, int>> Position() const;
		// False where the window system places windows itself.
		bool SetPosition(int x, int y);

		const std::string& Title() const { return m_Title; }
		void SetTitle(const std::string& title);

		bool IsFocused() const;
		void Focus();

		// Whether a key (hearth/platform/Keys.h `key`) or mouse button
		// (`mouse`) is held while this window has the input.
		bool IsKeyDown(int key) const;
		bool IsMouseButtonDown(int button) const;
		// The cursor in screen coordinates relative to the content area.
		std::pair<double, double> CursorPosition() const;

		CursorMode GetCursorMode() const;
		void SetCursorMode(CursorMode mode);
		// The cursor's shape over this window (nullopt: the default arrow).
		void SetCursorShape(std::optional<CursorShape> shape);

		// A Vulkan surface for this window. Returns the VkResult.
		int CreateVulkanSurface(VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) const;

		// The program's own pointer for this window (Hearth never reads it).
		void SetUserData(void* data) { m_UserData = data; }
		void* UserData() const { return m_UserData; }

	private:
		Platform& m_Platform;
		void* m_Handle = nullptr; // the window system's window
		void* m_Cursor = nullptr; // the shape's cursor, while one is set
		std::optional<CursorShape> m_CursorShape;
		std::string m_Title;
		void* m_UserData = nullptr;
	};

} // namespace hearth::platform
