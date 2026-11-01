#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace reborn {
class Logger {
public:
    explicit Logger(const std::filesystem::path& file);
    void write(std::string_view level, std::string_view event, std::string_view message);
private:
    std::mutex mutex_;
    std::ofstream stream_;
};

struct Settings {
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    double fixed_hz = 60.0;
    float master_volume = 0.5f;
};
Settings load_settings(const std::filesystem::path& file, Logger& logger);
void save_settings(const std::filesystem::path& file, const Settings& settings);

struct StepResult {
    uint32_t steps = 0;
    double alpha = 0;
    double dropped_seconds = 0;
};
// This is scheduling infrastructure, not a measured PS4 simulation rate.
class FixedStep {
public:
    explicit FixedStep(double hz = 60.0, uint32_t max_steps = 8);
    StepResult advance(double elapsed_seconds);
private:
    double step_seconds_;
    double accumulator_ = 0;
    uint32_t max_steps_;
};
}
