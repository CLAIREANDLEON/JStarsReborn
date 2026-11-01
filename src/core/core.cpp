#include "reborn/core.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

namespace reborn {
namespace {
std::string escape(std::string_view input) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    for (unsigned char c : input) {
        switch (c) {
        case '\\': output += "\\\\"; break;
        case '"': output += "\\\""; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (c < 32 || c >= 127) {
                // Byte escapes keep logs valid JSON even for invalid UTF-8 input.
                output += "\\u00";
                output += hex[c >> 4]; output += hex[c & 15];
            } else output += static_cast<char>(c);
        }
    }
    return output;
}
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return out.str();
}
std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}
template<class T> bool number(std::string_view text, T& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
void validate(const Settings& s) {
    if (s.width < 320 || s.width > 7680 || s.height < 240 || s.height > 4320)
        throw std::invalid_argument("resolution must be 320..7680 by 240..4320");
    if (!std::isfinite(s.fixed_hz) || s.fixed_hz < 30 || s.fixed_hz > 120)
        throw std::invalid_argument("fixed_hz must be finite and in 30..120");
    if (!std::isfinite(s.master_volume) || s.master_volume < 0 || s.master_volume > 1)
        throw std::invalid_argument("master_volume must be finite and in 0..1");
}
void ensure_parent(const std::filesystem::path& file) {
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path());
}
}

Logger::Logger(const std::filesystem::path& file) {
    ensure_parent(file);
    stream_.open(file, std::ios::binary | std::ios::app);
    if (!stream_) throw std::runtime_error("cannot open log file");
}
void Logger::write(std::string_view level, std::string_view event, std::string_view message) {
    std::lock_guard lock(mutex_);
    stream_ << "{\"time\":\"" << timestamp() << "\",\"level\":\"" << escape(level)
            << "\",\"event\":\"" << escape(event) << "\",\"message\":\"" << escape(message) << "\"}\n";
    stream_.flush();
    if (!stream_) throw std::runtime_error("log write failed");
}

Settings load_settings(const std::filesystem::path& file, Logger& logger) {
    Settings result;
    std::error_code error;
    if (!std::filesystem::exists(file, error) && !error) {
        logger.write("info", "settings_default", "settings file is absent; defaults loaded");
        return result;
    }
    try {
        if (error) throw std::system_error(error);
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read settings file");
        char buffer[4097];
        input.read(buffer, sizeof(buffer));
        const auto size = input.gcount();
        if (size > 4096) throw std::runtime_error("settings exceed 4096-byte limit");
        if (input.bad()) throw std::runtime_error("settings read failed");
        std::istringstream lines(std::string(buffer, static_cast<size_t>(size)));
        std::string line;
        std::set<std::string> keys;
        size_t line_number = 0;
        while (std::getline(lines, line)) {
            ++line_number;
            if (line.size() > 256) throw std::runtime_error("settings line exceeds 256 bytes");
            auto view = trim(line);
            if (view.empty() || view.front() == '#') continue;
            const auto separator = view.find('=');
            if (separator == std::string_view::npos) throw std::runtime_error("missing '=' at line " + std::to_string(line_number));
            const auto key = trim(view.substr(0, separator));
            const auto value = trim(view.substr(separator + 1));
            if (!keys.insert(std::string(key)).second) throw std::runtime_error("duplicate setting " + std::string(key));
            bool parsed = false;
            if (key == "width") parsed = number(value, result.width);
            else if (key == "height") parsed = number(value, result.height);
            else if (key == "vsync") {
                parsed = value == "0" || value == "1";
                result.vsync = value == "1";
            }
            else if (key == "fixed_hz") parsed = number(value, result.fixed_hz);
            else if (key == "master_volume") parsed = number(value, result.master_volume);
            else throw std::runtime_error("unknown setting " + std::string(key));
            if (!parsed) throw std::runtime_error("invalid value for " + std::string(key));
        }
        validate(result);
        logger.write("info", "settings_loaded", "settings validated");
        return result;
    } catch (const std::exception& e) {
        logger.write("warning", "settings_rejected", e.what());
        return Settings{};
    }
}

void save_settings(const std::filesystem::path& file, const Settings& s) {
    validate(s);
    ensure_parent(file);
    std::ostringstream text;
    text << "# JStarsReborn native foundation settings v1\n"
         << "width=" << s.width << "\nheight=" << s.height << "\nvsync=" << (s.vsync ? 1 : 0)
         << "\nfixed_hz=" << std::setprecision(std::numeric_limits<double>::max_digits10) << s.fixed_hz
         << "\nmaster_volume=" << std::setprecision(std::numeric_limits<float>::max_digits10) << s.master_volume << '\n';
    const auto bytes = text.str();
    static std::atomic<uint64_t> sequence{0};
    auto temporary = file;
#ifdef _WIN32
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence.fetch_add(1));
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "create settings temporary file");
    try {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "write settings");
        if (written != bytes.size()) throw std::runtime_error("short settings write");
        if (!FlushFileBuffers(handle)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "flush settings");
        if (!CloseHandle(handle)) { handle = INVALID_HANDLE_VALUE; throw std::runtime_error("close settings failed"); }
        handle = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "replace settings");
    } catch (...) {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        DeleteFileW(temporary.c_str());
        throw;
    }
#else
    temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." + std::to_string(sequence.fetch_add(1));
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) throw std::runtime_error("settings write failed");
        output.close();
        std::filesystem::rename(temporary, file);
    } catch (...) {
        std::error_code ignored; std::filesystem::remove(temporary, ignored); throw;
    }
#endif
}

FixedStep::FixedStep(double hz, uint32_t max_steps) : step_seconds_(0), max_steps_(max_steps) {
    if (!std::isfinite(hz) || hz < 1 || hz > 1000 || max_steps == 0 || max_steps > 1000000)
        throw std::invalid_argument("fixed step requires 1..1000 Hz and 1..1000000 max steps");
    step_seconds_ = 1.0 / hz;
}
StepResult FixedStep::advance(double elapsed_seconds) {
    if (!std::isfinite(elapsed_seconds) || elapsed_seconds < 0)
        throw std::invalid_argument("elapsed seconds must be finite and nonnegative");
    StepResult result;
    const double budget = step_seconds_ * max_steps_;
    double retained = elapsed_seconds;
    if (retained > budget) {
        retained = budget + std::fmod(elapsed_seconds, step_seconds_);
        result.dropped_seconds = std::max(0.0, elapsed_seconds - retained);
    }
    const double total = accumulator_ + retained;
    const auto available = static_cast<uint32_t>(std::floor(total / step_seconds_ + 1e-10));
    result.steps = std::min(available, max_steps_);
    result.dropped_seconds += (available - result.steps) * step_seconds_;
    accumulator_ = std::max(0.0, total - available * step_seconds_);
    result.alpha = std::clamp(accumulator_ / step_seconds_, 0.0, std::nextafter(1.0, 0.0));
    return result;
}
}
