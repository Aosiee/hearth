// hearth::App on a real device (Mesa's lavapipe in CI), offscreen: the
// presenter's offscreen image drawn by the swapchain graph and read back,
// the capture's PNG and sidecar, and the frame limit before the capture.

#include "hearth/App.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"
#include "hearth/rhi/vulkan/WindowPresenter.h"

#include "hamr/test.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hearth::app_gpu_tests {

	using namespace hearth::rhi;

	// Clears the window to one colour and counts its swapchain records.
	class ClearClient : public FrameClient
	{
	public:
		bool ClearColour(uint32_t, float rgba[4]) override
		{
			rgba[0] = 1.0f;
			rgba[1] = 0.0f;
			rgba[2] = 1.0f;
			rgba[3] = 1.0f;
			return true;
		}
		void RecordSwapchain(SwapchainGraph& swapchain, const FrameRecord&) override
		{
			// The depth imports too, as a UI pass's does.
			auto& pass = swapchain.Graph().AddPass("Test Pass");
			pass.Write(swapchain.Image(), TextureState::RenderTarget);
			pass.Write(swapchain.Depth(), TextureState::DepthWrite);
			SwapchainGraph* graph = &swapchain;
			pass.SetExecute([graph](CommandList& list, const rg::PassResources&) {
				graph->BeginRendering(list, false);
				list.EndRendering();
			});
			Records++;
		}
		// Written on the submit thread; read after a submit-idle wait.
		std::atomic<int> Records = 0;
	};

	AppDesc Offscreen(uint32_t width, uint32_t height)
	{
		AppDesc desc;
		desc.Title = "hearth app test";
		desc.Display = DisplayMode::Offscreen;
		desc.RenderTargetWidth = width;
		desc.RenderTargetHeight = height;
		desc.Workers = 2;
		return desc;
	}

	std::string ReadFile(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream text;
		text << in.rdbuf();
		return text.str();
	}

	TEST_CASE("app gpu: an offscreen frame draws into the presenter's image, read back as RGBA")
	{
		AppDesc desc = Offscreen(48, 24);
		desc.FrameLimit = 3;
		App app(desc);
		ClearClient client;
		app.AddFrameClient(client);
		CHECK(app.GetWindow() == nullptr);
		CHECK(app.GetPresenter().HasOffscreenImage());
		CHECK(app.FrameSize() == std::pair<uint32_t, uint32_t>(48u, 24u));
		CHECK(app.Run() == 0);
		// The read waits for the last frame's submit (Run returns once the
		// loop ends, with that frame still in flight).
		std::vector<uint8_t> rgba;
		REQUIRE(app.GetPresenter().ReadOffscreenImage(rgba));
		CHECK(client.Records == 3);
		REQUIRE(rgba.size() == 48u * 24u * 4u);
		for (size_t i = 0; i < rgba.size(); i += 4)
		{
			if (rgba[i] != 255 || rgba[i + 1] != 0 || rgba[i + 2] != 255 || rgba[i + 3] != 255)
			{
				FAIL("pixel " << i / 4 << " is not the clear colour");
				break;
			}
		}
		app.RemoveFrameClient(client);
	}

	TEST_CASE("app gpu: --capture-ui writes the PNG and its sidecar, and exits 0")
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / "hearth_app_gpu_capture";
		std::filesystem::create_directories(dir);
		const std::filesystem::path png = dir / "capture.png";
		std::filesystem::remove(png);
		std::filesystem::remove(png.string() + ".json");

		AppDesc desc = Offscreen(32, 16);
		desc.CaptureUiPath = png.string();
		desc.MinFrames = 4;
		desc.FrameLimit = 50;
		App app(desc);
		ClearClient client;
		app.AddFrameClient(client);
		int ready = 0;
		app.SetCaptureReady([&ready] { return ++ready >= 2; });
		CHECK(app.Run() == kExitCaptured);
		CHECK(!app.IsCapturing());
		// Frame 4 is the first the frame count allows; the predicate held at
		// its second ask.
		CHECK(client.Records == 5);
		app.RemoveFrameClient(client);

		REQUIRE(std::filesystem::exists(png));
		const std::string bytes = ReadFile(png);
		REQUIRE(bytes.size() > 8);
		CHECK(bytes.substr(1, 3) == "PNG");
		const std::string sidecar = ReadFile(png.string() + ".json");
		CHECK(sidecar.find("\"app\": \"hearth app test\"") != std::string::npos);
		CHECK(sidecar.find("\"width\": 32") != std::string::npos);
		CHECK(sidecar.find("\"height\": 16") != std::string::npos);
		CHECK(sidecar.find("\"frames\": 5") != std::string::npos);
		CHECK(sidecar.find("\"displayMode\": \"offscreen\"") != std::string::npos);
		const size_t hash = sidecar.find("\"blake3\": \"");
		REQUIRE(hash != std::string::npos);
		CHECK(sidecar.substr(hash + 11, 64).find_first_not_of("0123456789abcdef") == std::string::npos);
		std::filesystem::remove_all(dir);
	}

	TEST_CASE("app gpu: a frame limit before the capture exits 3")
	{
		AppDesc desc = Offscreen(16, 16);
		desc.CaptureUiPath = (std::filesystem::temp_directory_path() / "hearth_app_never.png").string();
		desc.MinFrames = 10;
		desc.FrameLimit = 4;
		App app(desc);
		CHECK(app.Run() == kExitNotSettled);
		CHECK(app.IsCapturing());
		CHECK(!std::filesystem::exists(desc.CaptureUiPath));
	}

	TEST_CASE("app gpu: Close stops the run with its exit code, and the loop's order is the App's")
	{
		AppDesc desc = Offscreen(16, 16);
		App app(desc);
		app.GetLoop().Add(Phase::Update, 100, "test.close", [&app](const Frame& frame) {
			if (frame.Index == 2)
				app.Close(7);
		});
		const std::vector<std::string> expected = {
			"Pump platform.pump", "Events events.dispatch", "Update test.close", "Present window.present",
			"EndFrame app.capture", "EndFrame app.stop",
		};
		CHECK(app.GetLoop().Order() == expected);
		CHECK(app.Run() == 7);
		CHECK(app.GetLoop().FrameIndex() == 2u);
	}

} // namespace hearth::app_gpu_tests
