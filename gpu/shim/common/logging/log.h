// JStarsReborn: replaces shadPS4's spdlog-based logger (common/logging/log.h) with a small fmt sink
// that writes into the runtime log. Level: JSTARS_GPU_LOG = trace|debug|info|warning|error (default info).
// The macro and class names are the upstream ones so vendored callers compile unchanged.
#pragma once
#include <string>
#include <string_view>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include "common/logging/classes.h"

namespace spdlog { enum class level : int { trace, debug, info, warn, err, critical, off }; }
namespace Common::Log {
using Level = spdlog::level;
bool ShouldLog(Level level);
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message);
inline void Flush() {}
} // namespace Common::Log

#define LOG_GENERIC(log_class, log_level, fmt_str, ...)                                            \
    do {                                                                                           \
        if (Common::Log::ShouldLog(log_level)) {                                                   \
            Common::Log::Write(log_class, log_level, __FILE__, __LINE__, __func__,                 \
                               fmt::format(fmt_str __VA_OPT__(, ) __VA_ARGS__));                   \
        }                                                                                          \
    } while (false)
#define LOG_TRACE(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::debug, __VA_ARGS__)
#define LOG_INFO(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::err, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::critical, __VA_ARGS__)
