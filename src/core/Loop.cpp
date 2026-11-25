#include "hearth/Loop.h"

#include "hearth/Assert.h"
#include "hearth/Log.h"
#include "hearth/Zone.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace hearth {

	HEARTH_LOG_CATEGORY(LogLoop)

	namespace {

		constexpr const char* kPhaseNames[kPhaseCount] = {
			"BeginFrame", "Pump", "Events", "Input", "Update", "Fixed", "Simulate", "Present", "EndFrame",
		};

		template <typename Call>
		void Invoke(const ClientOptions& options, const std::string& name, Call&& call)
		{
#if defined(HEARTH_ENABLE_ZONES)
			if (options.Zone)
			{
				// No source position: the zone is the client's, not this file's.
				const Zone zone(name.c_str(), "", 0);
				call();
				return;
			}
#else
			(void)options;
			(void)name;
#endif
			call();
		}

	} // namespace

	const char* PhaseName(Phase phase)
	{
		const uint32_t index = static_cast<uint32_t>(phase);
		return index < kPhaseCount ? kPhaseNames[index] : "?";
	}

	Loop::Loop()
		: m_Owner(std::this_thread::get_id())
	{
		const auto epoch = std::chrono::steady_clock::now();
		m_Clock = [epoch]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count(); };
		m_Sleep = [](double seconds) { std::this_thread::sleep_for(std::chrono::duration<double>(seconds)); };
	}

	bool Loop::IsOwnerThread() const
	{
		return std::this_thread::get_id() == m_Owner;
	}

	ClientId Loop::Insert(Client client)
	{
		HEARTH_ASSERT(IsOwnerThread(), "a loop's clients are added on the loop's thread");
		if (m_InFrame)
		{
			HEARTH_LOG_ERROR(LogLoop, "client '{}' refused: added inside a frame", client.Name);
			HEARTH_ASSERT(false, "a loop client is added inside a frame");
			return 0;
		}
		for (const Client& other : m_Clients)
		{
			if (other.Name == client.Name)
			{
				HEARTH_LOG_ERROR(LogLoop, "client '{}' refused: the name is taken", client.Name);
				HEARTH_ASSERT(false, "two loop clients share a name");
				return 0;
			}
			if (other.Where == client.Where && other.Key == client.Key)
			{
				HEARTH_LOG_ERROR(LogLoop, "client '{}' refused: {} order {} is '{}''s", client.Name, PhaseName(client.Where), client.Key, other.Name);
				HEARTH_ASSERT(false, "two loop clients share a phase's order key");
				return 0;
			}
		}
		client.Id = m_NextId++;
		const ClientId id = client.Id;
		const auto before = [](const Client& a, const Client& b) {
			return a.Where != b.Where ? a.Where < b.Where : a.Key < b.Key;
		};
		m_Clients.insert(std::upper_bound(m_Clients.begin(), m_Clients.end(), client, before), std::move(client));
		return id;
	}

	ClientId Loop::Add(Phase phase, int order, std::string name, FrameFn fn, ClientOptions options)
	{
		if (phase == Phase::Fixed)
		{
			HEARTH_LOG_ERROR(LogLoop, "client '{}' refused: Fixed clients are added with AddFixed", name);
			HEARTH_ASSERT(false, "a Fixed client is added with Add");
			return 0;
		}
		Client client;
		client.Where = phase;
		client.Key = order;
		client.Name = std::move(name);
		client.OnFrame = std::move(fn);
		client.Options = options;
		return Insert(std::move(client));
	}

	ClientId Loop::AddFixed(int order, std::string name, FixedFn fn, ClientOptions options)
	{
		// A paused frame runs no fixed step.
		options.RunsPaused = false;
		Client client;
		client.Where = Phase::Fixed;
		client.Key = order;
		client.Name = std::move(name);
		client.OnFixed = std::move(fn);
		client.Options = options;
		return Insert(std::move(client));
	}

	bool Loop::Remove(ClientId id)
	{
		HEARTH_ASSERT(IsOwnerThread(), "a loop's clients are removed on the loop's thread");
		if (m_InFrame)
		{
			HEARTH_LOG_ERROR(LogLoop, "client {} not removed: removed inside a frame", id);
			HEARTH_ASSERT(false, "a loop client is removed inside a frame");
			return false;
		}
		const auto it = std::find_if(m_Clients.begin(), m_Clients.end(), [id](const Client& client) { return client.Id == id; });
		if (it == m_Clients.end())
			return false;
		m_Clients.erase(it);
		return true;
	}

	void Loop::SetClock(std::function<double()> clock) { m_Clock = std::move(clock); }
	void Loop::SetSleep(std::function<void(double)> sleep) { m_Sleep = std::move(sleep); }

	void Loop::SetFixedStep(double seconds, uint32_t maxStepsPerFrame)
	{
		m_FixedStep = seconds > 0.0 ? seconds : 0.0;
		m_MaxFixedSteps = std::max<uint32_t>(maxStepsPerFrame, 1);
	}

	void Loop::SetFixedDelta(double seconds) { m_FixedDelta = seconds > 0.0 ? seconds : 0.0; }
	void Loop::SetFrameCap(double seconds) { m_FrameCap = seconds > 0.0 ? seconds : 0.0; }
	void Loop::SetPausedFrameCap(double seconds) { m_PausedFrameCap = seconds > 0.0 ? seconds : 0.0; }

	void Loop::Stop() { m_Stopping.store(true, std::memory_order_release); }
	bool Loop::IsStopping() const { return m_Stopping.load(std::memory_order_acquire); }

	void Loop::RunPhase(Phase phase, const Frame& frame)
	{
		// By index: a client may run paused frames, which walk the same list
		// (nothing adds or removes inside a frame).
		for (std::size_t i = 0; i < m_Clients.size(); ++i)
		{
			const Client& client = m_Clients[i];
			if (client.Where != phase || (frame.Paused && !client.Options.RunsPaused))
				continue;
			Invoke(client.Options, client.Name, [&]() { client.OnFrame(frame); });
		}
	}

	void Loop::RunFixed(const Frame& frame)
	{
		if (m_FixedStep <= 0.0)
			return;
		m_Accumulator += frame.Delta;
		uint32_t steps = 0;
		while (m_Accumulator >= m_FixedStep && steps < m_MaxFixedSteps)
		{
			const FixedStep step{ m_FixedTick, m_FixedStep, m_Accumulator, steps };
			for (std::size_t i = 0; i < m_Clients.size(); ++i)
			{
				const Client& client = m_Clients[i];
				if (client.Where == Phase::Fixed)
					Invoke(client.Options, client.Name, [&]() { client.OnFixed(frame, step); });
			}
			++m_FixedTick;
			m_Accumulator -= m_FixedStep;
			++steps;
		}
		// The time past the frame's last allowed step is dropped.
		if (m_Accumulator >= m_FixedStep)
			m_Accumulator = 0.0;
	}

	void Loop::CapFrame(double frameStart, double cap)
	{
		if (cap <= 0.0)
			return;
		const double spent = m_Clock() - frameStart;
		if (spent < cap)
			m_Sleep(cap - spent);
	}

	bool Loop::Run()
	{
		HEARTH_ASSERT(IsOwnerThread(), "a loop runs on its own thread");
		if (m_Running)
		{
			HEARTH_LOG_ERROR(LogLoop, "Run refused: the loop is running");
			return false;
		}
		m_Running = true;
		LogOrder();
		double last = m_Clock();
		while (!IsStopping())
		{
			const double start = m_Clock();
			const double delta = m_FixedDelta > 0.0 ? m_FixedDelta : start - last;
			last = start;
			m_LastDelta = delta;
			const Frame frame{ ++m_FrameIndex, start, delta, false };
			m_InFrame = true;
			for (uint32_t index = 0; index < kPhaseCount; ++index)
			{
				const Phase phase = static_cast<Phase>(index);
				if (phase == Phase::Fixed)
					RunFixed(frame);
				else
					RunPhase(phase, frame);
			}
			m_InFrame = false;
			if (!IsStopping())
				CapFrame(start, m_FrameCap);
		}
		m_Running = false;
		return true;
	}

	bool Loop::RunPaused(const std::function<bool()>& keepPaused)
	{
		HEARTH_ASSERT(IsOwnerThread(), "a loop runs on its own thread");
		if (m_Paused)
			return false;
		m_Paused = true;
		const bool wasInFrame = m_InFrame;
		double last = m_Clock();
		while (!IsStopping() && keepPaused())
		{
			const double start = m_Clock();
			const double delta = m_FixedDelta > 0.0 ? m_FixedDelta : start - last;
			last = start;
			const Frame frame{ ++m_FrameIndex, start, delta, true };
			m_InFrame = true;
			for (uint32_t index = 0; index < kPhaseCount; ++index)
			{
				const Phase phase = static_cast<Phase>(index);
				if (phase != Phase::Fixed && phase != Phase::Simulate)
					RunPhase(phase, frame);
			}
			if (!IsStopping())
				CapFrame(start, m_PausedFrameCap);
		}
		m_InFrame = wasInFrame;
		m_Paused = false;
		return true;
	}

	std::vector<std::string> Loop::Order() const
	{
		std::vector<std::string> order;
		order.reserve(m_Clients.size());
		for (const Client& client : m_Clients)
			order.push_back(std::string(PhaseName(client.Where)) + " " + client.Name);
		return order;
	}

	void Loop::LogOrder() const
	{
		HEARTH_LOG_INFO(LogLoop, "Loop: {} clients", m_Clients.size());
		for (const Client& client : m_Clients)
			HEARTH_LOG_INFO(LogLoop, "  {} {} {}{}", PhaseName(client.Where), client.Key, client.Name, client.Options.RunsPaused ? " (paused too)" : "");
	}

} // namespace hearth
