// hearth::App's command line: the shared flags, both spellings, the
// program's own arguments kept in order, and the flags it refuses.

#include "hearth/App.h"

#include "hamr/test.h"

#include <string>
#include <vector>

namespace hearth::app_args_tests {

	AppDesc Parse(std::vector<std::string> args, AppDesc defaults = {})
	{
		args.insert(args.begin(), "program");
		std::vector<char*> argv;
		for (std::string& arg : args)
			argv.push_back(arg.data());
		return App::ParseArgs((int)argv.size(), argv.data(), std::move(defaults));
	}

	TEST_CASE("app args: the shared flags set the description")
	{
		const AppDesc desc = Parse({ "--display-mode", "offscreen", "--render-target", "640x360", "--capture-ui", "out.png",
			"--fixed-dt", "0.016", "--frames", "120", "--min-frames", "8", "--platform", "null" });
		CHECK(desc.ArgError.empty());
		CHECK(desc.Display == DisplayMode::Offscreen);
		CHECK(desc.RenderTargetWidth == 640u);
		CHECK(desc.RenderTargetHeight == 360u);
		CHECK(desc.CaptureUiPath == "out.png");
		CHECK(desc.FixedDelta == hamr::test::Approx(0.016));
		CHECK(desc.FrameLimit == 120u);
		CHECK(desc.MinFrames == 8u);
		CHECK(desc.PlatformBackend == platform::Backend::Null);
		CHECK(desc.Args.empty());
	}

	TEST_CASE("app args: --flag=value reads like --flag value")
	{
		const AppDesc desc = Parse({ "--display-mode=hidden", "--frames=3", "--render-target=32x16" });
		CHECK(desc.ArgError.empty());
		CHECK(desc.Display == DisplayMode::Hidden);
		CHECK(desc.FrameLimit == 3u);
		CHECK(desc.RenderTargetWidth == 32u);
		CHECK(desc.RenderTargetHeight == 16u);
	}

	TEST_CASE("app args: the program's arguments stay, in order, and the defaults hold")
	{
		AppDesc defaults;
		defaults.Title = "Gallery";
		defaults.Width = 900;
		const AppDesc desc = Parse({ "--input-script", "s.json", "--frames", "2", "positional", "--other=1" }, defaults);
		CHECK(desc.Title == "Gallery");
		CHECK(desc.Width == 900u);
		CHECK(desc.FrameLimit == 2u);
		REQUIRE(desc.Args.size() == 4u);
		CHECK(desc.Args[0] == "--input-script");
		CHECK(desc.Args[1] == "s.json");
		CHECK(desc.Args[2] == "positional");
		CHECK(desc.Args[3] == "--other=1");
	}

	TEST_CASE("app args: a flag it cannot read is the run's error")
	{
		CHECK(!Parse({ "--display-mode", "sideways" }).ArgError.empty());
		CHECK(!Parse({ "--render-target", "640" }).ArgError.empty());
		CHECK(!Parse({ "--render-target", "0x10" }).ArgError.empty());
		CHECK(!Parse({ "--frames", "many" }).ArgError.empty());
		CHECK(!Parse({ "--fixed-dt", "-1" }).ArgError.empty());
		CHECK(!Parse({ "--capture-ui" }).ArgError.empty());
		CHECK(!Parse({ "--platform", "amiga" }).ArgError.empty());
		// The first error is the one reported.
		CHECK(Parse({ "--frames", "x", "--display-mode", "y" }).ArgError.rfind("--frames", 0) == 0);
	}

} // namespace hearth::app_args_tests
