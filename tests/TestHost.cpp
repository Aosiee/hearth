#include "TestHost.h"

#include <utility>

namespace hearth::test {

	std::vector<Call>& Calls()
	{
		static std::vector<Call> calls;
		return calls;
	}

	void Reset() { Calls().clear(); }

} // namespace hearth::test

namespace hearth::host {

	namespace {
		test::Call& Record(const char* what)
		{
			test::Call call;
			call.What = what;
			return test::Calls().emplace_back(std::move(call));
		}
	}

	void Log(LogLevel level, const char* category, const char* message)
	{
		test::Call& call = Record("log");
		call.Level = static_cast<int>(level);
		call.Category = category;
		call.Text = message;
	}

	bool AssertFailed(const char* expression, const char* message, const char* file, int line)
	{
		test::Call& call = Record("assert");
		call.Text = message;
		call.Detail = expression;
		call.File = file;
		call.Line = line;
		return false; // no trap: the suite keeps running
	}

	uint64_t BeginZone(const char* name, const char* file, int line)
	{
		const uint64_t handle = test::Calls().size() + 100;
		test::Call& call = Record("begin");
		call.Text = name;
		call.File = file;
		call.Line = line;
		call.Handle = handle;
		return handle;
	}

	void EndZone(uint64_t handle)
	{
		Record("end").Handle = handle;
	}

	void NoteGpu(const char* name, const char* driver)
	{
		test::Call& call = Record("gpu");
		call.Text = name;
		call.Detail = driver;
	}

} // namespace hearth::host
