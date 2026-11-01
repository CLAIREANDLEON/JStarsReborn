// JStarsReborn log sink: forwards to the runtime's rt_logf (set by jgpu_init) or stderr.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/logging/log.h"

extern "C" void (*jgpu_log_sink)(const char* line);

namespace Common::Log {
static Level ParseLevel() {
    const char* v = std::getenv("JSTARS_GPU_LOG");
    if (!v) return Level::info;
    if (!std::strcmp(v, "trace")) return Level::trace;
    if (!std::strcmp(v, "debug")) return Level::debug;
    if (!std::strcmp(v, "warning")) return Level::warn;
    if (!std::strcmp(v, "error")) return Level::err;
    return Level::info;
}
bool ShouldLog(Level level) {
    static const Level min = ParseLevel();
    return static_cast<int>(level) >= static_cast<int>(min);
}
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function, std::string&& message) {
    static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical", "off"};
    const char* base = std::strrchr(file, '/');
    const char* bs = std::strrchr(file, '\\');
    if (bs > base) base = bs;
    base = base ? base + 1 : file;
    std::string out = fmt::format("gpu[{}/{}] {}:{} {}: {}", log_class, names[static_cast<int>(level)], base, line, function, message);
    if (jgpu_log_sink) jgpu_log_sink(out.c_str());
    else std::fprintf(stderr, "%s\n", out.c_str());
}
} // namespace Common::Log
