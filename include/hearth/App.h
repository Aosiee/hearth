#pragma once

// The frame loop's owner for a program that is not an engine (an applet):
// the platform, the main window, a thread pool, the GPU device and the main
// window's presenter, the loop that drives them, and the capture flags such
// programs share.
//
//     int main(int argc, char** argv)
//     {
//         hearth::App app(hearth::App::ParseArgs(argc, argv, { .Title = "Hello", .Width = 640, .Height = 400 }));
//         app.AddFrameClient(myClient);        // what the window draws
//         app.AddEventHandler(myHandler);      // what the window's events do
//         return app.Run();
//     }
//
// The App's own loop clients are Pump 100 `platform.pump`, Events 100
// `events.dispatch`, Present 100 `window.present`, EndFrame 100
// `app.capture` and EndFrame 200 `app.stop`; a program adds its own
// (Update is the usual place) with keys that do not collide.
//
// Offscreen (`--display-mode offscreen`) there is no window and no display
// server: the platform is the null one and the presenter draws into an
// image of the render target's size, which `--capture-ui` writes out.

#include "hearth/Loop.h"
#include "hearth/platform/Event.h"
#include "hearth/platform/Platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace loom { class Pool; }

namespace hearth::platform { class Window; }

namespace hearth::rhi {
	class FrameClient;
	class VulkanRhiDevice;
	class WindowPresenter;
}

namespace hearth {

	enum class DisplayMode : uint8_t
	{
		Windowed,  // a visible window
		Hidden,    // an invisible window (a real swapchain; still needs a display server)
		Offscreen, // no window and no display server: the frames draw into an image
	};

	const char* DisplayModeName(DisplayMode mode);

	struct AppDesc
	{
		std::string Title = "Hearth";
		uint32_t Width = 1280;
		uint32_t Height = 720;
		uint32_t MinWidth = 0;
		uint32_t MinHeight = 0;
		bool Center = false;
		DisplayMode Display = DisplayMode::Windowed;
		platform::Backend PlatformBackend = platform::Backend::Any;
		// The offscreen image's size; 0 = Width x Height.
		uint32_t RenderTargetWidth = 0;
		uint32_t RenderTargetHeight = 0;
		// Where the capture's PNG goes (its sidecar beside it, `.json`);
		// empty = no capture. Offscreen only.
		std::string CaptureUiPath;
		// Every frame's delta pinned (a deterministic run); 0 = measured.
		double FixedDelta = 0.0;
		// The run ends after this many frames (0 = no limit).
		uint64_t FrameLimit = 0;
		// The capture waits at least this many frames (and always 2).
		uint64_t MinFrames = 0;
		bool Vsync = true;
		// The thread pool's workers (-1 = loom's default), when the App makes
		// the pool: a program with a loom pool already running keeps its own.
		int Workers = -1;
		// The sidecar's "app" (empty: the title).
		std::string AppName;
		// The command-line arguments ParseArgs did not take, in order.
		std::vector<std::string> Args;
		// A flag ParseArgs could not read; Run then exits 2.
		std::string ArgError;
	};

	// The exit codes Run returns besides the program's own.
	inline constexpr int kExitCaptured = 0;
	inline constexpr int kExitBadArguments = 2;
	inline constexpr int kExitNotSettled = 3; // the frame limit came before the capture

	class App
	{
	public:
		// The shared flags over `defaults`: --display-mode
		// windowed|hidden|offscreen, --render-target WxH, --capture-ui <png>,
		// --fixed-dt <seconds>, --frames N, --min-frames N and --platform
		// <backend> (each `--flag value` or `--flag=value`). Everything else
		// goes to Args.
		static AppDesc ParseArgs(int argc, char** argv, AppDesc defaults = {});

		// Creates the platform, the window, the pool, the device and the
		// presenter, and registers the App's loop clients. On the thread
		// that will run the loop.
		explicit App(AppDesc desc);
		// Stops the presenter, then destroys the device, the pool, the window
		// and the platform. Whatever draws through the device (a frame
		// client's resources) must be gone first.
		~App();
		App(const App&) = delete;
		App& operator=(const App&) = delete;

		// Runs the loop until Close (or the frame limit, or the capture).
		// Returns the exit code.
		int Run();
		// Ends the run after the current frame. Any thread.
		void Close();
		void Close(int exitCode);
		void SetExitCode(int exitCode) { m_ExitCode = exitCode; }
		int GetExitCode() const { return m_ExitCode; }

		const AppDesc& Desc() const { return m_Desc; }
		Loop& GetLoop() { return m_Loop; }
		platform::Platform& GetPlatform() { return *m_Platform; }
		// Null offscreen.
		platform::Window* GetWindow() const { return m_Window.get(); }
		rhi::VulkanRhiDevice& GetDevice() { return *m_Device; }
		rhi::WindowPresenter& GetPresenter() { return *m_Presenter; }
		bool IsOffscreen() const { return m_Desc.Display == DisplayMode::Offscreen; }
		// The main window's drawable size (the render target's offscreen).
		std::pair<uint32_t, uint32_t> FrameSize() const;

		// A handler for every window's events, called in registration order
		// at the Events phase; the first that returns true takes the event.
		// A close of the main window no handler takes ends the run.
		using EventFn = std::function<bool(const platform::Event&)>;
		using HandlerId = uint32_t;
		HandlerId AddEventHandler(EventFn handler);
		void RemoveEventHandler(HandlerId id);
		// Queues an event for the next Events phase as if the platform had
		// sent it (a script's input; a null Source is the offscreen window).
		void PostEvent(platform::Event event);

		// The main window's frame clients, in call order. Between frames.
		void AddFrameClient(rhi::FrameClient& client);
		void RemoveFrameClient(rhi::FrameClient& client);

		// What the capture waits for besides the frame count (a program that
		// settles over several frames). Default: nothing.
		void SetCaptureReady(std::function<bool()> ready) { m_CaptureReady = std::move(ready); }
		// A --capture-ui run that has not written its capture yet.
		bool IsCapturing() const { return m_Capturing; }

	private:
		void RegisterLoopClients();
		void DispatchEvents();
		void Capture(const Frame& frame);
		void ApplyClients();

		AppDesc m_Desc;
		std::unique_ptr<platform::Platform> m_Platform;
		std::unique_ptr<platform::Window> m_Window;
		std::unique_ptr<loom::Pool> m_Pool;
		std::unique_ptr<rhi::VulkanRhiDevice> m_Device;
		std::unique_ptr<rhi::WindowPresenter> m_Presenter;
		Loop m_Loop;

		std::vector<rhi::FrameClient*> m_FrameClients;
		std::vector<std::pair<HandlerId, EventFn>> m_EventHandlers;
		HandlerId m_NextHandlerId = 1;
		std::vector<platform::Event> m_Events;
		std::function<bool()> m_CaptureReady;
		bool m_Capturing = false;
		int m_ExitCode = 0;
	};

} // namespace hearth
