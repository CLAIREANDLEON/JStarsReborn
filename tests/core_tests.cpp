#include "reborn/core.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message) {
    require(std::abs(actual - expected) <= tolerance, message);
}
template<class Function> void rejects(Function function, const char* message) {
    bool threw = false;
    try { function(); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, message);
}
std::string read(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& file, const std::string& text) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << text;
    if (!stream) throw std::runtime_error("test fixture write failed");
}
struct Temporary {
    std::filesystem::path directory = std::filesystem::temp_directory_path() / ("reborn-core-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() { std::filesystem::create_directories(directory); }
    ~Temporary() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};
void timing_tests() {
    for (const int display_hz : {30, 60, 120, 144}) {
        reborn::FixedStep clock(60);
        uint64_t ticks = 0;
        double dropped = 0;
        for (int frame = 0; frame < display_hz * 20; ++frame) {
            auto result = clock.advance(1.0 / display_hz);
            ticks += result.steps; dropped += result.dropped_seconds;
            require(result.alpha >= 0 && result.alpha < 1, "interpolation fraction out of range");
        }
        require(ticks == 1200, "simulation tick count depends on display rate");
        near(dropped, 0, 1e-9, "normal cadence unexpectedly drops simulation time");
    }
    reborn::FixedStep irregular(60);
    double elapsed = 0, discarded = 0;
    uint64_t ticks = 0;
    double alpha = 0;
    for (int frame = 0; frame < 20000; ++frame) {
        const double dt = (3 + frame % 37) / 1000.0;
        const auto result = irregular.advance(dt);
        elapsed += dt; ticks += result.steps; discarded += result.dropped_seconds; alpha = result.alpha;
    }
    near(ticks / 60.0 + alpha / 60.0 + discarded, elapsed, 1e-7, "irregular timing loses elapsed time");
    reborn::FixedStep stall(60, 8);
    auto result = stall.advance(1.005);
    require(result.steps == 8, "stall did not cap catchup work");
    require(result.dropped_seconds > 0.8, "stall discarded time not reported");
    near(result.steps / 60.0 + result.alpha / 60.0 + result.dropped_seconds, 1.005, 1e-9, "stall elapsed time not accounted");
    const auto pending = stall.advance(0);
    require(pending.steps == 0, "stall retained unbounded catchup debt");
    const auto huge = stall.advance(std::numeric_limits<double>::max());
    require(huge.steps <= 8 && std::isfinite(huge.alpha) && std::isfinite(huge.dropped_seconds), "large finite elapsed time overflowed");
    rejects([&] { stall.advance(-1); }, "negative elapsed time accepted");
    rejects([&] { stall.advance(std::numeric_limits<double>::quiet_NaN()); }, "NaN elapsed time accepted");
    rejects([&] { stall.advance(std::numeric_limits<double>::infinity()); }, "infinite elapsed time accepted");
    rejects([] { reborn::FixedStep invalid(0); }, "zero rate accepted");
    rejects([] { reborn::FixedStep invalid(60, 0); }, "zero catchup budget accepted");
}
void settings_tests(const std::filesystem::path& directory) {
    reborn::Logger logger(directory / "settings-log.jsonl");
    const auto file = directory / "settings.ini";
    auto defaults = reborn::load_settings(file, logger);
    require(defaults.width == 1280 && defaults.height == 720 && defaults.vsync, "missing settings defaults wrong");
    reborn::Settings s;
    s.width = 3840; s.height = 2160; s.vsync = false; s.fixed_hz = 30; s.master_volume = 0.73f;
    reborn::save_settings(file, s);
    auto restored = reborn::load_settings(file, logger);
    require(restored.width == s.width && restored.height == s.height && restored.vsync == s.vsync, "settings display fields did not round-trip");
    require(restored.fixed_hz == s.fixed_hz && restored.master_volume == s.master_volume, "settings numeric fields did not round-trip");
    s.width = 1920;
    reborn::save_settings(file, s);
    require(reborn::load_settings(file, logger).width == 1920, "atomic replacement did not replace existing settings");
    const auto valid_content = read(file);
    s.width = 1;
    rejects([&] { reborn::save_settings(file, s); }, "out-of-bounds settings saved");
    require(read(file) == valid_content, "failed save modified previous configuration");
    for (const auto& malformed : std::vector<std::string>{
        "width=319\n", "width=7681\n", "height=239\n", "height=4321\n", "width=-1\n",
        "width=42949672960\n", "width=1920oops\n", "height=\n", "vsync=true\n",
        "fixed_hz=nan\n", "fixed_hz=inf\n", "fixed_hz=29\n", "fixed_hz=121\n",
        "master_volume=nan\n", "master_volume=-0.1\n", "master_volume=1.1\n",
        "width=1920\nwidth=2560\n", "width=1920\nunknown=1\n", "width 1920\n",
        std::string(257, 'x') + "\n", std::string(4097, '#'), std::string("width=1920\nheight=1080\0hidden", 29)
    }) {
        write(file, malformed);
        const auto rejected = reborn::load_settings(file, logger);
        require(rejected.width == 1280 && rejected.height == 720 && rejected.fixed_hz == 60 && rejected.master_volume == 0.5f,
                "malformed settings did not restore complete defaults");
    }
    write(file, "# comment\r\n width = 7680 \r\nheight=4320\nvsync=0\nfixed_hz=120\nmaster_volume=1\n");
    const auto maximum = reborn::load_settings(file, logger);
    require(maximum.width == 7680 && maximum.height == 4320 && maximum.fixed_hz == 120 && maximum.master_volume == 1 && !maximum.vsync,
            "valid settings boundaries rejected");
    const auto logs = read(directory / "settings-log.jsonl");
    require(logs.find("settings_rejected") != std::string::npos, "invalid settings lack diagnostics");
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        require(entry.path().filename().string().find(".tmp.") == std::string::npos, "settings temporary file leaked");
}
void logging_tests(const std::filesystem::path& directory) {
    const auto file = directory / "threads.jsonl";
    {
        reborn::Logger logger(file);
        logger.write("info", "escape", std::string("quote\" slash\\ newline\n tab\t") + '\1' + static_cast<char>(0xff));
        std::atomic<bool> failed = false;
        std::vector<std::thread> threads;
        for (int thread = 0; thread < 6; ++thread) {
            threads.emplace_back([&, thread] {
                try {
                    for (int index = 0; index < 100; ++index)
                        logger.write("info", "concurrent", std::to_string(thread) + ":" + std::to_string(index));
                } catch (...) { failed = true; }
            });
        }
        for (auto& thread : threads) thread.join();
        require(!failed, "concurrent logger write failed");
    }
    const auto text = read(file);
    require(text.find("quote\\\" slash\\\\ newline\\n tab\\t\\u0001\\u00ff") != std::string::npos, "JSON escaping failed");
    std::istringstream lines(text);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        ++count;
        require(line.starts_with("{\"time\":\"") && line.ends_with("\"}"), "concurrent log records interleaved");
        require(line.find("Z\",\"level\":\"") != std::string::npos, "log timestamp missing UTC marker");
    }
    require(count == 601, "concurrent log records lost");
}
}
int main() {
    try {
        Temporary temporary;
        timing_tests();
        settings_tests(temporary.directory);
        logging_tests(temporary.directory);
        std::cout << "PASS: " << checks << " core checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
