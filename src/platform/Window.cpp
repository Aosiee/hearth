// The Window on GLFW. Every callback turns into an Event on the Platform's
// queue, in the order GLFW calls them.

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "hearth/platform/Window.h"

#include "hearth/Assert.h"

#include <algorithm>

namespace hearth::platform {

	namespace {

		GLFWwindow* Glfw(void* handle) { return static_cast<GLFWwindow*>(handle); }

		Window& Owner(GLFWwindow* window) { return *static_cast<Window*>(glfwGetWindowUserPointer(window)); }

		Action ActionOf(int glfwAction)
		{
			return glfwAction == GLFW_PRESS ? Action::Press : glfwAction == GLFW_REPEAT ? Action::Repeat : Action::Release;
		}

		int GlfwShapeOf(CursorShape shape)
		{
			switch (shape)
			{
			case CursorShape::Arrow: return GLFW_ARROW_CURSOR;
			case CursorShape::IBeam: return GLFW_IBEAM_CURSOR;
			case CursorShape::Crosshair: return GLFW_CROSSHAIR_CURSOR;
			case CursorShape::Hand: return GLFW_HAND_CURSOR;
			case CursorShape::ResizeEW: return GLFW_HRESIZE_CURSOR;
			case CursorShape::ResizeNS: return GLFW_VRESIZE_CURSOR;
			}
			return GLFW_ARROW_CURSOR;
		}

	} // namespace

	// The callbacks reach the Platform through the window: a free function
	// holds no state, so the queue needs no global.
	struct WindowCallbacks
	{
		static void Push(GLFWwindow* handle, Event event)
		{
			Window& window = Owner(handle);
			event.Source = &window;
			window.GetPlatform().m_Events.push_back(std::move(event));
		}

		static void Install(GLFWwindow* w)
		{
			glfwSetWindowSizeCallback(w, [](GLFWwindow* h, int width, int height) {
				Event e;
				e.Type = EventType::WindowResize;
				e.Width = width;
				e.Height = height;
				Push(h, std::move(e));
			});
			glfwSetWindowPosCallback(w, [](GLFWwindow* h, int x, int y) {
				Event e;
				e.Type = EventType::WindowMove;
				e.X = x;
				e.Y = y;
				Push(h, std::move(e));
			});
			glfwSetWindowCloseCallback(w, [](GLFWwindow* h) {
				Event e;
				e.Type = EventType::WindowClose;
				Push(h, std::move(e));
			});
			glfwSetWindowFocusCallback(w, [](GLFWwindow* h, int focused) {
				Event e;
				e.Type = EventType::WindowFocus;
				e.Focused = focused != 0;
				Push(h, std::move(e));
			});
			glfwSetWindowContentScaleCallback(w, [](GLFWwindow* h, float x, float y) {
				Event e;
				e.Type = EventType::WindowContentScale;
				e.ScaleX = x;
				e.ScaleY = y;
				Push(h, std::move(e));
			});
			glfwSetCursorEnterCallback(w, [](GLFWwindow* h, int entered) {
				Event e;
				e.Type = EventType::CursorEnter;
				e.Entered = entered != 0;
				Push(h, std::move(e));
			});
			glfwSetKeyCallback(w, [](GLFWwindow* h, int key, int scancode, int action, int mods) {
				Event e;
				e.Type = EventType::Key;
				e.KeyCode = key;
				e.Scancode = scancode;
				e.KeyAction = ActionOf(action);
				e.Mods = mods;
				Push(h, std::move(e));
			});
			glfwSetCharCallback(w, [](GLFWwindow* h, unsigned int codepoint) {
				Event e;
				e.Type = EventType::Char;
				e.Codepoint = codepoint;
				Push(h, std::move(e));
			});
			glfwSetMouseButtonCallback(w, [](GLFWwindow* h, int button, int action, int mods) {
				Event e;
				e.Type = EventType::MouseButton;
				e.Button = button;
				e.KeyAction = ActionOf(action);
				e.Mods = mods;
				Push(h, std::move(e));
			});
			glfwSetScrollCallback(w, [](GLFWwindow* h, double x, double y) {
				Event e;
				e.Type = EventType::Scroll;
				e.ScrollX = x;
				e.ScrollY = y;
				Push(h, std::move(e));
			});
			glfwSetCursorPosCallback(w, [](GLFWwindow* h, double x, double y) {
				Event e;
				e.Type = EventType::CursorMove;
				e.CursorX = x;
				e.CursorY = y;
				Push(h, std::move(e));
			});
			glfwSetDropCallback(w, [](GLFWwindow* h, int count, const char** paths) {
				Event e;
				e.Type = EventType::Drop;
				e.Paths.assign(paths, paths + count);
				Push(h, std::move(e));
			});
		}
	};

	Window::Window(Platform& platform, const WindowDesc& desc) : m_Platform(platform), m_Title(desc.Title)
	{
		// The program creates its own surface: no client API context. Both
		// hints are set for every window, since GLFW keeps a hint until the
		// next glfwInit.
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_VISIBLE, desc.Visible ? GLFW_TRUE : GLFW_FALSE);
		GLFWwindow* window = glfwCreateWindow((int)desc.Width, (int)desc.Height, desc.Title.c_str(), nullptr, nullptr);
		HEARTH_ASSERT(window != nullptr, "the window system refused the window");
		m_Handle = window;
		if (!window)
			return;
		++m_Platform.m_WindowCount;

		if (desc.MinWidth > 0 || desc.MinHeight > 0)
			glfwSetWindowSizeLimits(window, desc.MinWidth > 0 ? (int)desc.MinWidth : GLFW_DONT_CARE,
				desc.MinHeight > 0 ? (int)desc.MinHeight : GLFW_DONT_CARE, GLFW_DONT_CARE, GLFW_DONT_CARE);
		// Wayland gives a program no position: the compositor places the
		// window, and a placement attempt is an error there.
		if (desc.Center && m_Platform.GetBackend() != Backend::Wayland)
		{
			if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
			{
				int x = 0, y = 0, w = 0, h = 0;
				glfwGetMonitorWorkarea(monitor, &x, &y, &w, &h);
				glfwSetWindowPos(window, x + (w - (int)desc.Width) / 2, y + (h - (int)desc.Height) / 2);
			}
		}

		glfwSetWindowUserPointer(window, this);
		WindowCallbacks::Install(window);
	}

	Window::~Window()
	{
		if (!m_Handle)
			return;
		// No event may outlive the window it names.
		std::vector<Event>& events = m_Platform.m_Events;
		events.erase(std::remove_if(events.begin(), events.end(), [this](const Event& e) { return e.Source == this; }), events.end());
		glfwDestroyWindow(Glfw(m_Handle));
		if (m_Cursor)
			glfwDestroyCursor(static_cast<GLFWcursor*>(m_Cursor));
		--m_Platform.m_WindowCount;
	}

	std::pair<int, int> Window::Size() const
	{
		int w = 0, h = 0;
		glfwGetWindowSize(Glfw(m_Handle), &w, &h);
		return { w, h };
	}

	void Window::SetSize(int width, int height)
	{
		glfwSetWindowSize(Glfw(m_Handle), width, height);
	}

	std::pair<int, int> Window::FramebufferSize() const
	{
		int w = 0, h = 0;
		glfwGetFramebufferSize(Glfw(m_Handle), &w, &h);
		return { w, h };
	}

	std::pair<float, float> Window::ContentScale() const
	{
		float x = 1.0f, y = 1.0f;
		glfwGetWindowContentScale(Glfw(m_Handle), &x, &y);
		return { x, y };
	}

	std::optional<std::pair<int, int>> Window::Position() const
	{
		if (!m_Platform.GetCaps().CanTrackGlobalCursor)
			return std::nullopt;
		int x = 0, y = 0;
		glfwGetWindowPos(Glfw(m_Handle), &x, &y);
		return std::make_pair(x, y);
	}

	bool Window::SetPosition(int x, int y)
	{
		if (!m_Platform.GetCaps().CanPlaceWindows)
			return false;
		glfwSetWindowPos(Glfw(m_Handle), x, y);
		return true;
	}

	void Window::SetTitle(const std::string& title)
	{
		m_Title = title;
		glfwSetWindowTitle(Glfw(m_Handle), title.c_str());
	}

	bool Window::IsFocused() const
	{
		return glfwGetWindowAttrib(Glfw(m_Handle), GLFW_FOCUSED) == GLFW_TRUE;
	}

	void Window::Focus()
	{
		glfwFocusWindow(Glfw(m_Handle));
	}

	bool Window::IsKeyDown(int key) const
	{
		const int state = glfwGetKey(Glfw(m_Handle), key);
		return state == GLFW_PRESS || state == GLFW_REPEAT;
	}

	bool Window::IsMouseButtonDown(int button) const
	{
		return glfwGetMouseButton(Glfw(m_Handle), button) == GLFW_PRESS;
	}

	std::pair<double, double> Window::CursorPosition() const
	{
		double x = 0.0, y = 0.0;
		glfwGetCursorPos(Glfw(m_Handle), &x, &y);
		return { x, y };
	}

	CursorMode Window::GetCursorMode() const
	{
		switch (glfwGetInputMode(Glfw(m_Handle), GLFW_CURSOR))
		{
		case GLFW_CURSOR_HIDDEN: return CursorMode::Hidden;
		case GLFW_CURSOR_DISABLED: return CursorMode::Disabled;
		default: return CursorMode::Normal;
		}
	}

	void Window::SetCursorMode(CursorMode mode)
	{
		const int value = mode == CursorMode::Hidden ? GLFW_CURSOR_HIDDEN : mode == CursorMode::Disabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
		glfwSetInputMode(Glfw(m_Handle), GLFW_CURSOR, value);
	}

	void Window::SetCursorShape(std::optional<CursorShape> shape)
	{
		if (shape == m_CursorShape)
			return;
		m_CursorShape = shape;
		// Destroying the window's current cursor gives it the default arrow
		// until the new one is set.
		if (m_Cursor)
			glfwDestroyCursor(static_cast<GLFWcursor*>(m_Cursor));
		m_Cursor = nullptr;
		if (!shape)
		{
			glfwSetCursor(Glfw(m_Handle), nullptr);
			return;
		}
		m_Cursor = glfwCreateStandardCursor(GlfwShapeOf(*shape));
		glfwSetCursor(Glfw(m_Handle), static_cast<GLFWcursor*>(m_Cursor));
	}

	int Window::CreateVulkanSurface(VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) const
	{
		return (int)glfwCreateWindowSurface(instance, Glfw(m_Handle), allocator, surface);
	}

} // namespace hearth::platform
