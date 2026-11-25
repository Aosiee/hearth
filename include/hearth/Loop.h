#pragma once

// The frame loop: fixed phases, each running the clients registered into it
// in the order of their keys. The loop owns the frame's clock, its counter
// and delta, the fixed-step accumulator and the frame cap; what a frame
// does is the clients'.
//
//     hearth::Loop loop;
//     loop.SetFixedStep(1.0 / 60.0, 8);
//     loop.Add(hearth::Phase::Update, 100, "game.update", [&](const hearth::Frame& frame) { game.Update(frame.Delta); });
//     loop.AddFixed(100, "game.step", [&](const hearth::Frame&, const hearth::FixedStep& step) { game.Step(step.Step); });
//     loop.Run(); // until loop.Stop()
//
// A frame runs the phases in their declared order; Fixed runs its clients
// once per fixed step (none, one or several a frame). Paused frames
// (RunPaused) run every phase but Fixed and Simulate, and in them only the
// clients registered with RunsPaused: a debugger that stops a script keeps
// the window alive this way.
//
// Everything but Stop is called on the thread that constructed the loop.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace hearth {

	enum class Phase : uint8_t
	{
		BeginFrame, // per-frame bookkeeping before anything reads input
		Pump,       // the window system's events
		Events,     // the program's event dispatch
		Input,      // input devices sampled (after the events are dispatched)
		Update,     // the per-frame update
		Fixed,      // once per fixed step
		Simulate,   // simulation driven by the frame's delta
		Present,    // the windows' frames
		EndFrame,   // per-frame bookkeeping after the present
	};

	inline constexpr uint32_t kPhaseCount = 9;

	const char* PhaseName(Phase phase);

	// The frame a client runs in.
	struct Frame
	{
		uint64_t Index = 0;  // frames begun, this one included (the first is 1)
		double Time = 0.0;   // the clock at the frame's start, in seconds
		double Delta = 0.0;  // seconds since the previous frame's start (or the pinned delta)
		bool Paused = false; // a paused frame (RunPaused)
	};

	// The fixed step a Fixed client runs in.
	struct FixedStep
	{
		uint64_t Tick = 0;         // fixed steps before this one, since the loop began
		double Step = 0.0;         // the step's length, in seconds
		double Accumulator = 0.0;  // the time still to step, this step included
		uint32_t StepInFrame = 0;  // 0 for the frame's first step
	};

	struct ClientOptions
	{
		bool RunsPaused = false; // also runs in paused frames (never for Fixed)
		bool Zone = true;        // a profiler zone named by the client around each call
	};

	using ClientId = uint32_t; // 0 = refused

	class Loop
	{
	public:
		using FrameFn = std::function<void(const Frame&)>;
		using FixedFn = std::function<void(const Frame&, const FixedStep&)>;

		Loop();
		Loop(const Loop&) = delete;
		Loop& operator=(const Loop&) = delete;

		// Registers a client; refused (0, with an error logged) inside a
		// frame, for Phase::Fixed (AddFixed is its form), for an order key
		// another client of the phase holds, or for a name another client
		// holds.
		ClientId Add(Phase phase, int order, std::string name, FrameFn fn, ClientOptions options = {});
		ClientId AddFixed(int order, std::string name, FixedFn fn, ClientOptions options = {});
		// Refused inside a frame.
		bool Remove(ClientId id);

		// The clock in seconds, monotonic (default: std::chrono::steady_clock).
		void SetClock(std::function<double()> clock);
		// How the frame cap waits (default: std::this_thread::sleep_for).
		void SetSleep(std::function<void(double seconds)> sleep);
		// The fixed step and the most steps a frame runs; time beyond them
		// is dropped, so a long frame does not spiral. A step of 0 runs no
		// fixed steps.
		void SetFixedStep(double seconds, uint32_t maxStepsPerFrame);
		// Every frame's delta pinned (a deterministic run); 0 = measured.
		void SetFixedDelta(double seconds);
		// The shortest frame: the loop sleeps off what a frame leaves of it,
		// measured from the frame's start (0 = uncapped). Paused frames use
		// their own (default 1/60 s).
		void SetFrameCap(double seconds);
		void SetPausedFrameCap(double seconds);

		// Runs frames until Stop. Logs the clients' order first. Refused
		// (false) while running.
		bool Run();
		// Runs paused frames until `keepPaused` returns false or the loop
		// stops; called from a client inside a frame (or outside Run). The
		// running frame's own delta and fixed accumulator are untouched.
		// Refused (false) while already paused.
		bool RunPaused(const std::function<bool()>& keepPaused);
		// Ends the loop after the running frame (which completes, without
		// the cap's wait). Sticky: a stop before Run runs no frame. Any
		// thread.
		void Stop();

		bool IsStopping() const;
		bool IsRunning() const { return m_Running; }
		bool InFrame() const { return m_InFrame; }
		bool IsPaused() const { return m_Paused; }
		uint64_t FrameIndex() const { return m_FrameIndex; }
		uint64_t FixedTick() const { return m_FixedTick; }
		// The running loop's last frame delta (paused frames leave it).
		double LastDelta() const { return m_LastDelta; }

		// The clients in the order a frame runs them, "<Phase> <name>".
		std::vector<std::string> Order() const;
		// Logs the order (LogLoop), one line per client.
		void LogOrder() const;

	private:
		struct Client
		{
			ClientId Id = 0;
			Phase Where = Phase::BeginFrame;
			int Key = 0;
			std::string Name;
			FrameFn OnFrame;
			FixedFn OnFixed;
			ClientOptions Options;
		};

		ClientId Insert(Client client);
		bool IsOwnerThread() const;
		void RunPhase(Phase phase, const Frame& frame);
		void RunFixed(const Frame& frame);
		void CapFrame(double frameStart, double cap);

		std::vector<Client> m_Clients; // sorted by (phase, key)
		ClientId m_NextId = 1;
		std::function<double()> m_Clock;
		std::function<void(double)> m_Sleep;
		double m_FixedStep = 0.0;
		uint32_t m_MaxFixedSteps = 1;
		double m_FixedDelta = 0.0;
		double m_FrameCap = 0.0;
		double m_PausedFrameCap = 1.0 / 60.0;
		double m_Accumulator = 0.0;
		double m_LastDelta = 0.0;
		uint64_t m_FrameIndex = 0;
		uint64_t m_FixedTick = 0;
		bool m_Running = false;
		bool m_InFrame = false;
		bool m_Paused = false;
		std::atomic<bool> m_Stopping{ false };
		std::thread::id m_Owner;
	};

} // namespace hearth
