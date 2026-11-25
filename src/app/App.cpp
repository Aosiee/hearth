#include "hearth/App.h"

#include "hearth/Assert.h"
#include "hearth/Log.h"
#include "hearth/Version.h"
#include "hearth/platform/Window.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"
#include "hearth/rhi/vulkan/WindowPresenter.h"

#include "loom/loom.h"

#include <blake3.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string_view>

namespace hearth {

	HEARTH_LOG_CATEGORY(LogApp)

	namespace {

		bool ParseUint(std::string_view text, uint64_t& out)
		{
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
			return error == std::errc() && end == text.data() + text.size();
		}

		bool ParseSize(std::string_view text, uint32_t& width, uint32_t& height)
		{
			const size_t x = text.find('x');
			uint64_t w = 0, h = 0;
			if (x == std::string_view::npos || !ParseUint(text.substr(0, x), w) || !ParseUint(text.substr(x + 1), h)
				|| w == 0 || h == 0 || w > 16384 || h > 16384)
				return false;
			width = (uint32_t)w;
			height = (uint32_t)h;
			return true;
		}

		std::string JsonString(std::string_view text)
		{
			std::string out = "\"";
			for (const char c : text)
			{
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\t': out += "\\t"; break;
				default:
					if ((unsigned char)c < 0x20)
					{
						char escape[8];
						std::snprintf(escape, sizeof(escape), "\\u%04x", (unsigned)(unsigned char)c);
						out += escape;
					}
					else
						out += c;
				}
			}
			return out + "\"";
		}

	} // namespace

	const char* DisplayModeName(DisplayMode mode)
	{
		switch (mode)
		{
		case DisplayMode::Windowed: return "windowed";
		case DisplayMode::Hidden: return "hidden";
		case DisplayMode::Offscreen: return "offscreen";
		}
		return "?";
	}

	AppDesc App::ParseArgs(int argc, char** argv, AppDesc defaults)
	{
		AppDesc desc = std::move(defaults);
		std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + std::max(argc, 0));
		for (size_t i = 0; i < args.size(); i++)
		{
			std::string flag = args[i];
			std::string value;
			bool hasValue = false;
			if (const size_t eq = flag.find('='); flag.rfind("--", 0) == 0 && eq != std::string::npos)
			{
				value = flag.substr(eq + 1);
				flag.resize(eq);
				hasValue = true;
			}
			const auto takeValue = [&]() -> bool {
				if (hasValue)
					return true;
				if (i + 1 >= args.size())
					return false;
				value = args[++i];
				return true;
			};
			const auto fail = [&](const char* what) {
				if (desc.ArgError.empty())
					desc.ArgError = flag + ": " + what;
			};

			if (flag == "--display-mode")
			{
				if (!takeValue())
					fail("needs windowed, hidden or offscreen");
				else if (value == "windowed")
					desc.Display = DisplayMode::Windowed;
				else if (value == "hidden")
					desc.Display = DisplayMode::Hidden;
				else if (value == "offscreen")
					desc.Display = DisplayMode::Offscreen;
				else
					fail("needs windowed, hidden or offscreen");
			}
			else if (flag == "--render-target")
			{
				if (!takeValue() || !ParseSize(value, desc.RenderTargetWidth, desc.RenderTargetHeight))
					fail("needs WxH");
			}
			else if (flag == "--capture-ui")
			{
				if (!takeValue() || value.empty())
					fail("needs a path");
				else
					desc.CaptureUiPath = value;
			}
			else if (flag == "--fixed-dt")
			{
				char* end = nullptr;
				const double seconds = takeValue() ? std::strtod(value.c_str(), &end) : 0.0;
				if (!end || *end != '\0' || !(seconds > 0.0))
					fail("needs a positive number of seconds");
				else
					desc.FixedDelta = seconds;
			}
			else if (flag == "--frames" || flag == "--min-frames")
			{
				uint64_t count = 0;
				if (!takeValue() || !ParseUint(value, count))
					fail("needs a count");
				else
					(flag == "--frames" ? desc.FrameLimit : desc.MinFrames) = count;
			}
			else if (flag == "--platform")
			{
				const std::optional<platform::Backend> backend = takeValue() ? platform::ParseBackend(value) : std::nullopt;
				if (!backend)
					fail("needs a platform backend");
				else
					desc.PlatformBackend = *backend;
			}
			else
			{
				desc.Args.push_back(args[i]);
			}
		}
		return desc;
	}

	App::App(AppDesc desc)
		: m_Desc(std::move(desc))
	{
		const bool offscreen = IsOffscreen();
		platform::PlatformDesc platformDesc;
		platformDesc.Requested = offscreen ? platform::Backend::Null : m_Desc.PlatformBackend;
		m_Platform = platform::Platform::Create(platformDesc);
		if (!offscreen)
		{
			platform::WindowDesc windowDesc;
			windowDesc.Title = m_Desc.Title;
			windowDesc.Width = m_Desc.Width;
			windowDesc.Height = m_Desc.Height;
			windowDesc.MinWidth = m_Desc.MinWidth;
			windowDesc.MinHeight = m_Desc.MinHeight;
			windowDesc.Center = m_Desc.Center;
			windowDesc.Visible = m_Desc.Display == DisplayMode::Windowed;
			m_Window = std::make_unique<platform::Window>(*m_Platform, windowDesc);
		}

		// A program that already runs a pool keeps it (loom has one at a
		// time); otherwise the App owns one.
		if (!loom::IsInitialised())
		{
			loom::LoomConfig poolConfig;
			poolConfig.workerCount = m_Desc.Workers;
			m_Pool = std::make_unique<loom::Pool>(poolConfig);
		}

		m_Device = std::make_unique<rhi::VulkanRhiDevice>();
		m_Device->SetPipelineCacheFile("");
		m_Device->EnsureDevice(m_Window.get());

		rhi::WindowPresenterDesc presenterDesc;
		presenterDesc.Window = m_Window.get();
		presenterDesc.IsMainWindow = true;
		presenterDesc.OffscreenWidth = m_Desc.RenderTargetWidth ? m_Desc.RenderTargetWidth : m_Desc.Width;
		presenterDesc.OffscreenHeight = m_Desc.RenderTargetHeight ? m_Desc.RenderTargetHeight : m_Desc.Height;
		presenterDesc.OffscreenImage = offscreen;
		presenterDesc.Owner = this;
		m_Presenter = std::make_unique<rhi::WindowPresenter>(*m_Device, presenterDesc);
		m_Presenter->SetVsync(m_Desc.Vsync);

		m_Capturing = !m_Desc.CaptureUiPath.empty();
		m_Loop.SetFixedDelta(m_Desc.FixedDelta);
		RegisterLoopClients();
	}

	App::~App()
	{
		// The last frames finish, then everything that holds device objects
		// goes before the device.
		m_Presenter->Stop();
		m_Presenter.reset();
		m_Device->WaitIdle();
		m_Device->ShutdownDevice();
		m_Device.reset();
		m_Pool.reset();
		m_Window.reset();
		m_Platform.reset();
	}

	std::pair<uint32_t, uint32_t> App::FrameSize() const
	{
		if (!m_Window)
			return { m_Desc.RenderTargetWidth ? m_Desc.RenderTargetWidth : m_Desc.Width,
				m_Desc.RenderTargetHeight ? m_Desc.RenderTargetHeight : m_Desc.Height };
		const auto [width, height] = m_Window->FramebufferSize();
		return { (uint32_t)std::max(width, 0), (uint32_t)std::max(height, 0) };
	}

	App::HandlerId App::AddEventHandler(EventFn handler)
	{
		const HandlerId id = m_NextHandlerId++;
		m_EventHandlers.emplace_back(id, std::move(handler));
		return id;
	}

	void App::RemoveEventHandler(HandlerId id)
	{
		HEARTH_ASSERT(!m_Loop.InFrame(), "an event handler is removed between frames");
		std::erase_if(m_EventHandlers, [id](const auto& entry) { return entry.first == id; });
	}

	void App::PostEvent(platform::Event event)
	{
		m_Events.push_back(std::move(event));
	}

	void App::AddFrameClient(rhi::FrameClient& client)
	{
		HEARTH_ASSERT(!m_Loop.InFrame(), "a frame client is added between frames");
		m_FrameClients.push_back(&client);
		ApplyClients();
	}

	void App::RemoveFrameClient(rhi::FrameClient& client)
	{
		HEARTH_ASSERT(!m_Loop.InFrame(), "a frame client is removed between frames");
		// No record may still be calling it.
		m_Presenter->WaitForSubmitIdle();
		std::erase(m_FrameClients, &client);
		ApplyClients();
	}

	void App::ApplyClients()
	{
		m_Presenter->SetClients(m_FrameClients);
	}

	void App::Close()
	{
		m_Loop.Stop();
	}

	void App::Close(int exitCode)
	{
		m_ExitCode = exitCode;
		m_Loop.Stop();
	}

	void App::RegisterLoopClients()
	{
		m_Loop.Add(Phase::Pump, 100, "platform.pump", [this](const Frame&) {
			m_Platform->Pump();
			std::vector<platform::Event> events = m_Platform->TakeEvents();
			m_Events.insert(m_Events.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
		}, { .RunsPaused = true });
		m_Loop.Add(Phase::Events, 100, "events.dispatch", [this](const Frame&) { DispatchEvents(); }, { .RunsPaused = true });
		m_Loop.Add(Phase::Present, 100, "window.present", [this](const Frame&) { m_Presenter->Present(); }, { .RunsPaused = true });
		m_Loop.Add(Phase::EndFrame, 100, "app.capture", [this](const Frame& frame) { Capture(frame); });
		m_Loop.Add(Phase::EndFrame, 200, "app.stop", [this](const Frame& frame) {
			if (m_Desc.FrameLimit == 0 || frame.Index < m_Desc.FrameLimit || m_Loop.IsStopping())
				return;
			if (m_Capturing)
			{
				HEARTH_LOG_ERROR(LogApp, "render-target: the frame limit ({}) came before the capture", m_Desc.FrameLimit);
				Close(kExitNotSettled);
				return;
			}
			Close();
		}, { .RunsPaused = true });
	}

	void App::DispatchEvents()
	{
		// An event a handler posts waits for the next frame: the list is
		// swapped out first.
		std::vector<platform::Event> events;
		events.swap(m_Events);
		for (const platform::Event& event : events)
		{
			bool handled = false;
			for (const auto& [id, handler] : m_EventHandlers)
			{
				if (handler(event))
				{
					handled = true;
					break;
				}
			}
			if (!handled && event.Type == platform::EventType::WindowClose && event.Source == m_Window.get())
				Close();
		}
	}

	void App::Capture(const Frame& frame)
	{
		if (!m_Capturing || m_Loop.IsStopping())
			return;
		if (frame.Index < std::max<uint64_t>(m_Desc.MinFrames, 2) || (m_CaptureReady && !m_CaptureReady()))
			return;
		std::vector<uint8_t> rgba;
		if (!m_Presenter->ReadOffscreenImage(rgba))
		{
			HEARTH_LOG_ERROR(LogApp, "render-target: the offscreen image could not be read back");
			Close(kExitNotSettled);
			return;
		}
		m_Capturing = false;
		const auto [width, height] = FrameSize();
		const std::string& path = m_Desc.CaptureUiPath;
		if (!stbi_write_png(path.c_str(), (int)width, (int)height, 4, rgba.data(), (int)width * 4))
		{
			HEARTH_LOG_ERROR(LogApp, "render-target: '{}' could not be written", path);
			Close(kExitBadArguments);
			return;
		}

		blake3_hasher hasher;
		blake3_hasher_init(&hasher);
		blake3_hasher_update(&hasher, rgba.data(), rgba.size());
		uint8_t hash[BLAKE3_OUT_LEN];
		blake3_hasher_finalize(&hasher, hash, BLAKE3_OUT_LEN);
		std::string hex;
		for (const uint8_t byte : hash)
		{
			char pair[3];
			std::snprintf(pair, sizeof(pair), "%02x", byte);
			hex += pair;
		}
		std::ofstream sidecar(path + ".json");
		sidecar << "{\n"
				<< "  \"app\": " << JsonString(m_Desc.AppName.empty() ? m_Desc.Title : m_Desc.AppName) << ",\n"
				<< "  \"image\": " << JsonString(path) << ",\n"
				<< "  \"blake3\": \"" << hex << "\",\n"
				<< "  \"width\": " << width << ",\n"
				<< "  \"height\": " << height << ",\n"
				<< "  \"frames\": " << frame.Index << ",\n"
				<< "  \"displayMode\": \"" << DisplayModeName(m_Desc.Display) << "\",\n"
				<< "  \"hearth\": \"" << HEARTH_VERSION_STRING << "\"\n"
				<< "}\n";
		HEARTH_LOG_INFO(LogApp, "render-target: wrote '{}' ({} frames)", path, frame.Index);
		Close(kExitCaptured);
	}

	int App::Run()
	{
		if (!m_Desc.ArgError.empty())
		{
			HEARTH_LOG_ERROR(LogApp, "{}", m_Desc.ArgError);
			return kExitBadArguments;
		}
		if (m_Capturing && !IsOffscreen())
		{
			HEARTH_LOG_ERROR(LogApp, "--capture-ui needs --display-mode offscreen");
			return kExitBadArguments;
		}
		HEARTH_LOG_INFO(LogApp, "{}: {} {}x{}", m_Desc.Title, DisplayModeName(m_Desc.Display), FrameSize().first, FrameSize().second);
		m_Loop.Run();
		return m_ExitCode;
	}

} // namespace hearth
