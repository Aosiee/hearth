#pragma once

// Logging through the host (hearth/Host.h). A category is a name;
// HEARTH_LOG_CATEGORY declares one as a function returning it, so a header
// can declare a category for every file that includes it:
//
//     HEARTH_LOG_CATEGORY(LogVulkan)
//     HEARTH_LOG_INFO(LogVulkan, "{} queue families", count);
//
// The format is std::format's. The category object is constexpr, so it is
// read-only data, not a global anything can write.

#include "hearth/Host.h"

#include <format>
#include <string>

namespace hearth {

	struct LogCategory
	{
		const char* Name;
	};

	namespace detail {
		template <typename... Args>
		void Log(host::LogLevel level, const LogCategory& category, std::format_string<Args...> format, Args&&... args)
		{
			const std::string message = std::format(format, std::forward<Args>(args)...);
			host::Log(level, category.Name, message.c_str());
		}
	}

} // namespace hearth

#define HEARTH_LOG_CATEGORY(name) \
	inline const ::hearth::LogCategory& name() { static constexpr ::hearth::LogCategory s_Category{ #name }; return s_Category; }

#define HEARTH_LOG_TRACE(category, ...) ::hearth::detail::Log(::hearth::host::LogLevel::Trace, (category)(), __VA_ARGS__)
#define HEARTH_LOG_DEBUG(category, ...) ::hearth::detail::Log(::hearth::host::LogLevel::Debug, (category)(), __VA_ARGS__)
#define HEARTH_LOG_INFO(category, ...) ::hearth::detail::Log(::hearth::host::LogLevel::Info, (category)(), __VA_ARGS__)
#define HEARTH_LOG_WARN(category, ...) ::hearth::detail::Log(::hearth::host::LogLevel::Warn, (category)(), __VA_ARGS__)
#define HEARTH_LOG_ERROR(category, ...) ::hearth::detail::Log(::hearth::host::LogLevel::Error, (category)(), __VA_ARGS__)
