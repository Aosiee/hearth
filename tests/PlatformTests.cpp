// The platform on GLFW's null window system: windows, the events they
// queue, the clock, the clipboard and the one-per-process rule. Each case
// makes its own Platform (and ends it), so the cases run in any order.

#include "hearth/platform/Platform.h"
#include "hearth/platform/Window.h"

#include "hamr/test.h"

#include <memory>

using namespace hearth::platform;

namespace {

	std::unique_ptr<Platform> NullPlatform()
	{
		PlatformDesc desc;
		desc.Requested = Backend::Null;
		return Platform::Create(desc);
	}

	WindowDesc Desc(uint32_t width = 320, uint32_t height = 200)
	{
		WindowDesc desc;
		desc.Title = "test";
		desc.Width = width;
		desc.Height = height;
		return desc;
	}

} // namespace

TEST_CASE("backend: names parse and print")
{
	CHECK(ParseBackend("x11") == Backend::X11);
	CHECK(ParseBackend("Wayland") == Backend::Wayland);
	CHECK(ParseBackend("native") == Backend::Any);
	CHECK(ParseBackend("any") == Backend::Any);
	CHECK(ParseBackend("null") == Backend::Null);
	CHECK_FALSE(ParseBackend("amiga").has_value());
	CHECK(std::string(BackendName(Backend::Wayland)) == "Wayland");
}

TEST_CASE("platform: the null window system starts, and reports itself")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	CHECK(platform->GetBackend() == Backend::Null);
	const Caps caps = platform->GetCaps();
	CHECK(caps.CanCreateOSWindows);
	CHECK_FALSE(caps.CanPlaceWindows);
	CHECK_FALSE(caps.CanTrackGlobalCursor);
	CHECK(platform->WindowCount() == 0);
}

TEST_CASE("platform: there is one at a time, and another after it ends")
{
	{
		auto first = NullPlatform();
		REQUIRE(first != nullptr);
		CHECK(NullPlatform() == nullptr);
	}
	CHECK(NullPlatform() != nullptr);
}

TEST_CASE("platform: the clock runs forward from its start")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	const double a = platform->Time();
	const double b = platform->Time();
	CHECK(a >= 0.0);
	CHECK(b >= a);
}

TEST_CASE("platform: the clipboard keeps what is set")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	platform->SetClipboard("copied text");
	CHECK(platform->GetClipboard() == "copied text");
}

TEST_CASE("window: its size, title and the window count")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	{
		Window window(*platform, Desc(320, 200));
		CHECK(platform->WindowCount() == 1);
		CHECK(window.Size() == std::make_pair(320, 200));
		CHECK(window.FramebufferSize() == std::make_pair(320, 200));
		CHECK(window.Title() == "test");
		window.SetTitle("renamed");
		CHECK(window.Title() == "renamed");
		// The null window system places windows itself, as Wayland does.
		CHECK_FALSE(window.Position().has_value());
		CHECK_FALSE(window.SetPosition(10, 10));
	}
	CHECK(platform->WindowCount() == 0);
}

TEST_CASE("window: a resize arrives as an event naming the window")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	Window window(*platform, Desc());
	platform->TakeEvents();
	window.SetSize(640, 480);
	platform->Pump();
	bool resized = false;
	for (const Event& e : platform->TakeEvents())
	{
		CHECK(e.Source == &window);
		if (e.Type == EventType::WindowResize)
		{
			resized = true;
			CHECK(e.Width == 640);
			CHECK(e.Height == 480);
		}
	}
	CHECK(resized);
	CHECK(platform->TakeEvents().empty());
}

TEST_CASE("window: focus moves between windows as events, in order")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	Window a(*platform, Desc());
	Window b(*platform, Desc());
	a.Focus();
	platform->TakeEvents();
	b.Focus();
	const std::vector<Event> events = platform->TakeEvents();
	REQUIRE(events.size() == 2);
	CHECK(events[0].Type == EventType::WindowFocus);
	CHECK(events[0].Source == &a);
	CHECK_FALSE(events[0].Focused);
	CHECK(events[1].Source == &b);
	CHECK(events[1].Focused);
	CHECK(b.IsFocused());
	CHECK_FALSE(a.IsFocused());
}

TEST_CASE("window: its pending events go with it")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	Window kept(*platform, Desc());
	{
		Window gone(*platform, Desc());
		platform->TakeEvents();
		gone.SetSize(100, 100);
		kept.SetSize(200, 200);
	}
	for (const Event& e : platform->TakeEvents())
		CHECK(e.Source == &kept);
}

TEST_CASE("window: input state, the cursor mode and shapes")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	Window window(*platform, Desc());
	CHECK_FALSE(window.IsKeyDown(65));
	CHECK_FALSE(window.IsMouseButtonDown(0));
	CHECK(window.GetCursorMode() == CursorMode::Normal);
	window.SetCursorMode(CursorMode::Disabled);
	CHECK(window.GetCursorMode() == CursorMode::Disabled);
	window.SetCursorMode(CursorMode::Normal);
	window.SetCursorShape(CursorShape::IBeam);
	window.SetCursorShape(CursorShape::Hand);
	window.SetCursorShape(std::nullopt);
}

TEST_CASE("window: the program's own pointer")
{
	auto platform = NullPlatform();
	REQUIRE(platform != nullptr);
	Window window(*platform, Desc());
	int owner = 0;
	CHECK(window.UserData() == nullptr);
	window.SetUserData(&owner);
	CHECK(window.UserData() == &owner);
}
