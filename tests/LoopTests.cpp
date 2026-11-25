// The frame loop on a test clock: the phase and key order, the frame's
// counter and delta, the fixed steps and their cap, paused frames, the stop,
// the frame cap, the zones and the refusals.

// HEARTH_ENABLE_ZONES comes from hearth::core, as the library was built.
#include "hearth/Loop.h"

#include "TestHost.h"
#include "hamr/test.h"

#include <string>
#include <vector>

using hearth::ClientOptions;
using hearth::FixedStep;
using hearth::Frame;
using hearth::Loop;
using hearth::Phase;

namespace {

	// A clock the test moves, and a sleep that records its waits and moves
	// the clock by them.
	struct TestTime
	{
		double Now = 0.0;
		std::vector<double> Sleeps;

		void Attach(Loop& loop)
		{
			loop.SetClock([this]() { return Now; });
			loop.SetSleep([this](double seconds) { Sleeps.push_back(seconds); Now += seconds; });
		}
	};

	// A refusal logs an error in every build (and asserts where asserts
	// are on).
	bool Refused()
	{
		for (const hearth::test::Call& call : hearth::test::Calls())
			if (call.What == "log" && call.Category == "LogLoop" && call.Level == static_cast<int>(hearth::host::LogLevel::Error))
				return true;
		return false;
	}

	// Stops the loop at the end of frame `last`.
	void StopAfter(Loop& loop, uint64_t last)
	{
		loop.Add(Phase::EndFrame, 1000000, "test.stop", [&loop, last](const Frame& frame) {
			if (frame.Index >= last)
				loop.Stop();
		});
	}

} // namespace

TEST_CASE("loop: the phases run in order, each phase's clients by their keys")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	std::vector<std::string> ran;
	auto record = [&ran](const char* name) { return [&ran, name](const Frame&) { ran.push_back(name); }; };
	loop.Add(Phase::EndFrame, 100, "end", record("end"));
	loop.Add(Phase::Update, 200, "update.b", record("update.b"));
	loop.Add(Phase::Update, 100, "update.a", record("update.a"));
	loop.Add(Phase::BeginFrame, 100, "begin", record("begin"));
	loop.Add(Phase::Present, 100, "present", record("present"));
	loop.Add(Phase::Pump, 100, "pump", record("pump"));
	loop.Add(Phase::Events, 100, "events", record("events"));
	loop.Add(Phase::Input, 100, "input", record("input"));
	loop.Add(Phase::Simulate, 100, "simulate", record("simulate"));
	StopAfter(loop, 1);

	const std::vector<std::string> order = loop.Order();
	const std::vector<std::string> expected = {
		"BeginFrame begin", "Pump pump", "Events events", "Input input", "Update update.a", "Update update.b",
		"Simulate simulate", "Present present", "EndFrame end", "EndFrame test.stop",
	};
	CHECK(order == expected);

	REQUIRE(loop.Run());
	CHECK(ran == std::vector<std::string>{ "begin", "pump", "events", "input", "update.a", "update.b", "simulate", "present", "end" });
	CHECK_FALSE(loop.IsRunning());
	CHECK_FALSE(Refused());
}

TEST_CASE("loop: Run logs the order first")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.Add(Phase::Update, 100, "update", [](const Frame&) {}, { .RunsPaused = true });
	StopAfter(loop, 1);
	loop.Run();
	std::vector<std::string> lines;
	for (const hearth::test::Call& call : hearth::test::Calls())
		if (call.What == "log" && call.Category == "LogLoop")
			lines.push_back(call.Text);
	REQUIRE(lines.size() >= 3);
	CHECK(lines[0] == "Loop: 2 clients");
	CHECK(lines[1] == "  Update 100 update (paused too)");
	CHECK(lines[2] == "  EndFrame 1000000 test.stop");
}

TEST_CASE("loop: a frame's number, time and delta")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	time.Now = 10.0;
	std::vector<Frame> frames;
	loop.Add(Phase::Update, 100, "update", [&](const Frame& frame) {
		frames.push_back(frame);
		time.Now += 0.25; // the frame's work
	});
	StopAfter(loop, 3);
	loop.Run();
	REQUIRE(frames.size() == 3);
	CHECK(frames[0].Index == 1);
	CHECK(frames[0].Time == 10.0);
	CHECK(frames[0].Delta == 0.0); // the first frame starts as Run does
	CHECK(frames[1].Index == 2);
	CHECK(frames[1].Delta == hamr::test::Approx(0.25));
	CHECK(frames[2].Index == 3);
	CHECK(frames[2].Time == hamr::test::Approx(10.5));
	CHECK_FALSE(frames[2].Paused);
	CHECK(loop.FrameIndex() == 3);
	CHECK(loop.LastDelta() == hamr::test::Approx(0.25));
}

TEST_CASE("loop: a pinned delta replaces the measured one")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.SetFixedDelta(1.0 / 30.0);
	std::vector<double> deltas;
	loop.Add(Phase::Update, 100, "update", [&](const Frame& frame) { deltas.push_back(frame.Delta); time.Now += 1.0; });
	StopAfter(loop, 2);
	loop.Run();
	CHECK(deltas == std::vector<double>{ 1.0 / 30.0, 1.0 / 30.0 });
}

TEST_CASE("loop: the fixed steps run on the accumulated delta")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.SetFixedStep(0.25, 8);
	loop.SetFixedDelta(0.625);
	std::vector<FixedStep> steps;
	std::vector<uint64_t> frameOfStep;
	loop.AddFixed(200, "fixed.b", [&](const Frame&, const FixedStep&) { frameOfStep.push_back(0); });
	loop.AddFixed(100, "fixed.a", [&](const Frame& frame, const FixedStep& step) { steps.push_back(step); frameOfStep.push_back(frame.Index); });
	StopAfter(loop, 2);
	loop.Run();
	// 0.625 a frame: two steps, then three (0.125 carried + 0.625).
	REQUIRE(steps.size() == 5);
	CHECK(steps[0].Tick == 0);
	CHECK(steps[0].StepInFrame == 0);
	CHECK(steps[0].Accumulator == 0.625);
	CHECK(steps[1].Tick == 1);
	CHECK(steps[1].StepInFrame == 1);
	CHECK(steps[1].Accumulator == 0.375);
	CHECK(steps[2].Tick == 2);
	CHECK(steps[2].StepInFrame == 0);
	CHECK(steps[2].Accumulator == 0.75);
	CHECK(steps[4].Tick == 4);
	CHECK(steps[4].Accumulator == 0.25);
	CHECK(steps[4].Step == 0.25);
	// a before b, every step
	CHECK(frameOfStep == std::vector<uint64_t>{ 1, 0, 1, 0, 2, 0, 2, 0, 2, 0 });
	CHECK(loop.FixedTick() == 5);
}

TEST_CASE("loop: a long frame runs the most steps allowed and drops the rest")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.SetFixedStep(0.01, 3);
	std::vector<uint64_t> stepsPerFrame;
	loop.AddFixed(100, "fixed", [&](const Frame& frame, const FixedStep&) {
		if (stepsPerFrame.size() < frame.Index)
			stepsPerFrame.resize(frame.Index, 0);
		++stepsPerFrame[frame.Index - 1];
	});
	loop.Add(Phase::Update, 100, "work", [&](const Frame& frame) { time.Now += frame.Index == 1 ? 1.0 : 0.015; });
	StopAfter(loop, 3);
	loop.Run();
	// Frame 2 measures the 1 s frame 1 took: three steps, the rest dropped.
	// Frame 3: 0.015, one step (nothing carried).
	REQUIRE(stepsPerFrame.size() == 3);
	CHECK(stepsPerFrame[0] == 0);
	CHECK(stepsPerFrame[1] == 3);
	CHECK(stepsPerFrame[2] == 1);
}

TEST_CASE("loop: paused frames run only the clients that run paused, and no fixed step or simulate")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.SetFixedStep(0.125, 8);
	loop.SetFixedDelta(0.25);
	loop.SetPausedFrameCap(0.0);
	std::vector<std::string> ran;
	auto record = [&ran](const char* name) { return [&ran, name](const Frame& frame) { ran.push_back(std::string(frame.Paused ? "paused " : "") + name); }; };
	loop.Add(Phase::BeginFrame, 100, "begin", record("begin"), { .RunsPaused = true });
	loop.Add(Phase::Pump, 100, "pump", record("pump"), { .RunsPaused = true });
	loop.Add(Phase::Update, 100, "update", record("update"), { .RunsPaused = true });
	loop.Add(Phase::Update, 200, "worlds", record("worlds"));
	loop.AddFixed(100, "fixed", [&ran](const Frame& frame, const FixedStep&) { ran.push_back(std::string(frame.Paused ? "paused " : "") + "fixed"); }, { .RunsPaused = true });
	loop.Add(Phase::Simulate, 100, "simulate", [&](const Frame& frame) {
		ran.push_back(frame.Paused ? "paused simulate" : "simulate");
		if (frame.Index != 1)
			return;
		int paused = 0;
		CHECK(loop.RunPaused([&]() { return paused++ < 2; }));
		CHECK_FALSE(loop.IsPaused());
		CHECK(loop.InFrame());
	}, { .RunsPaused = true });
	loop.Add(Phase::Present, 100, "present", [&](const Frame& frame) {
		ran.push_back(std::string(frame.Paused ? "paused " : "") + "present");
		if (frame.Paused)
		{
			CHECK(loop.IsPaused());
			CHECK_FALSE(loop.RunPaused([]() { return true; })); // not nested
		}
	}, { .RunsPaused = true });
	loop.Add(Phase::EndFrame, 100, "end", record("end"));
	StopAfter(loop, 4);
	loop.Run();
	const std::vector<std::string> expected = {
		"begin", "pump", "update", "worlds", "fixed", "fixed", "simulate",
		"paused begin", "paused pump", "paused update", "paused present",
		"paused begin", "paused pump", "paused update", "paused present",
		"present", "end",
		"begin", "pump", "update", "worlds", "fixed", "fixed", "simulate", "present", "end",
	};
	CHECK(ran == expected);
	// The paused frames count: frames 2 and 3, the outer frame's next is 4.
	CHECK(loop.FrameIndex() == 4);
	CHECK(loop.FixedTick() == 4);
	CHECK(loop.LastDelta() == 0.25);
}

TEST_CASE("loop: a stop inside a frame finishes the frame without the cap's wait")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.SetFrameCap(0.5);
	std::vector<std::string> ran;
	loop.Add(Phase::Update, 100, "update", [&](const Frame& frame) {
		ran.push_back("update");
		if (frame.Index == 2)
			loop.Stop();
	});
	loop.Add(Phase::Present, 100, "present", [&](const Frame&) { ran.push_back("present"); });
	loop.Run();
	CHECK(ran == std::vector<std::string>{ "update", "present", "update", "present" });
	CHECK(time.Sleeps.size() == 1); // frame 1's; frame 2 stopped
	CHECK(loop.IsStopping());
	CHECK(loop.Run()); // sticky: no frame runs
	CHECK(loop.FrameIndex() == 2);
}

TEST_CASE("loop: a stop before Run runs no frame")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	int frames = 0;
	loop.Add(Phase::Update, 100, "update", [&](const Frame&) { ++frames; });
	loop.Stop();
	CHECK(loop.Run());
	CHECK(frames == 0);
}

TEST_CASE("loop: the frame cap sleeps what the frame leaves, from its start")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.SetFrameCap(0.1);
	loop.SetPausedFrameCap(0.05);
	loop.Add(Phase::BeginFrame, 100, "work", [&](const Frame& frame) { time.Now += frame.Paused ? 0.01 : 0.03; }, { .RunsPaused = true });
	loop.Add(Phase::Simulate, 100, "pause", [&](const Frame& frame) {
		if (frame.Index == 1)
		{
			int paused = 0;
			loop.RunPaused([&]() { return paused++ < 1; });
		}
	});
	StopAfter(loop, 3);
	loop.Run();
	// Frame 1: 0.03 of work, then the paused frame 2 (0.01: sleeps 0.04 of
	// its 0.05), then frame 1's own cap measured from its start: 0.08 spent,
	// 0.02 left. Frame 3 stops, so it does not wait.
	REQUIRE(time.Sleeps.size() == 2);
	CHECK(time.Sleeps[0] == hamr::test::Approx(0.04));
	CHECK(time.Sleeps[1] == hamr::test::Approx(0.02));
}

TEST_CASE("loop: each client runs in a zone named by it unless it opts out")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	loop.Add(Phase::Update, 100, "zoned", [](const Frame&) {});
	loop.Add(Phase::EndFrame, 100, "bare", [&loop](const Frame&) { loop.Stop(); }, { .Zone = false });
	loop.Run();
	std::vector<std::string> zones;
	for (const hearth::test::Call& call : hearth::test::Calls())
		if (call.What == "begin")
			zones.push_back(call.Text);
#if defined(HEARTH_ENABLE_ZONES)
	CHECK(zones == std::vector<std::string>{ "zoned" });
#else
	CHECK(zones.empty());
#endif
}

TEST_CASE("loop: refusals - a taken key or name, Fixed through Add, changes inside a frame")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	const hearth::ClientId first = loop.Add(Phase::Update, 100, "a", [](const Frame&) {});
	CHECK(first != 0);
	CHECK_FALSE(Refused());

	CHECK(loop.Add(Phase::Update, 100, "b", [](const Frame&) {}) == 0);
	CHECK(Refused());
	hearth::test::Reset();
	CHECK(loop.Add(Phase::Present, 100, "a", [](const Frame&) {}) == 0);
	CHECK(Refused());
	hearth::test::Reset();
	CHECK(loop.Add(Phase::Fixed, 100, "f", [](const Frame&) {}) == 0);
	CHECK(Refused());
	hearth::test::Reset();
	// The same key in another phase is fine.
	CHECK(loop.Add(Phase::Present, 100, "p", [](const Frame&) {}) != 0);
	CHECK(loop.AddFixed(100, "fixed", [](const Frame&, const FixedStep&) {}) != 0);
	CHECK_FALSE(Refused());

	hearth::ClientId added = 1;
	bool removed = true;
	loop.Add(Phase::EndFrame, 100, "inside", [&](const Frame&) {
		added = loop.Add(Phase::EndFrame, 200, "late", [](const Frame&) {});
		removed = loop.Remove(first);
		loop.Stop();
	});
	loop.Run();
	CHECK(added == 0);
	CHECK_FALSE(removed);
	CHECK(Refused());
	CHECK(loop.Order().size() == 4);
}

TEST_CASE("loop: a removed client no longer runs")
{
	hearth::test::Reset();
	Loop loop;
	TestTime time;
	time.Attach(loop);
	int runs = 0;
	const hearth::ClientId id = loop.Add(Phase::Update, 100, "update", [&](const Frame&) { ++runs; });
	CHECK(loop.Remove(id));
	CHECK_FALSE(loop.Remove(id));
	StopAfter(loop, 2);
	loop.Run();
	CHECK(runs == 0);
	CHECK(loop.Order() == std::vector<std::string>{ "EndFrame test.stop" });
}

TEST_CASE("loop: the phase names")
{
	CHECK(std::string(hearth::PhaseName(Phase::BeginFrame)) == "BeginFrame");
	CHECK(std::string(hearth::PhaseName(Phase::Fixed)) == "Fixed");
	CHECK(std::string(hearth::PhaseName(Phase::EndFrame)) == "EndFrame");
	CHECK(std::string(hearth::PhaseName(static_cast<Phase>(hearth::kPhaseCount))) == "?");
}
