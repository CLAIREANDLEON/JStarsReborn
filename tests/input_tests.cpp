#include "reborn/input.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}
bool close(float a, float b) { return std::abs(a - b) < 0.00001F; }
}

int main() {
    using reborn::normalize_axis;
    using reborn::normalize_trigger;
    check(normalize_axis(-32768, 0) == -1.0F, "negative integer endpoint");
    check(normalize_axis(32767, 0) == 1.0F, "positive integer endpoint");
    check(normalize_axis(0, 0) == 0.0F, "zero axis");
    check(normalize_axis(-7849, 7849) == 0.0F, "negative dead-zone boundary");
    check(normalize_axis(7849, 7849) == 0.0F, "positive dead-zone boundary");
    check(normalize_axis(-7850, 7849) < 0.0F, "negative outside dead zone");
    check(normalize_axis(7850, 7849) > 0.0F, "positive outside dead zone");
    check(normalize_axis(-32768, 32766) == -1.0F, "largest dead-zone negative endpoint");
    check(normalize_axis(32767, 32766) == 1.0F, "largest dead-zone positive endpoint");
    check(close(normalize_axis(-16384, 0), -0.5F), "negative midpoint");
    for (const int invalid : {-1, 32767, 32768, std::numeric_limits<int>::max()}) {
        bool rejected = false;
        try { (void)normalize_axis(0, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "invalid dead zone rejected");
    }
    for (const int deadzone : {0, 7849, 8689, 32766}) {
        float previous = -1.0F;
        for (int value = -32768; value <= 32767; ++value) {
            const auto sample = normalize_axis(static_cast<std::int16_t>(value), deadzone);
            if (!std::isfinite(sample) || sample < -1.0F || sample > 1.0F || sample < previous) {
                check(false, "all axis samples are finite, bounded, monotonic");
                break;
            }
            previous = sample;
        }
    }
    check(normalize_trigger(0) == 0.0F, "zero trigger");
    check(normalize_trigger(30) == 0.0F, "trigger dead-zone boundary");
    check(normalize_trigger(31) > 0.0F, "trigger outside dead zone");
    check(normalize_trigger(255) == 1.0F, "trigger upper endpoint");
    check(normalize_trigger(255, 255) == 0.0F, "fully disabled trigger");
    for (int deadzone = 0; deadzone <= 255; ++deadzone) {
        float previous = 0.0F;
        for (int value = 0; value <= 255; ++value) {
            const auto sample = normalize_trigger(static_cast<std::uint8_t>(value),
                                                   static_cast<std::uint8_t>(deadzone));
            if (!std::isfinite(sample) || sample < 0.0F || sample > 1.0F || sample < previous) {
                check(false, "all trigger samples are finite, bounded, monotonic");
                break;
            }
            previous = sample;
        }
    }
    reborn::InputSystem input;
    for (const auto& pad : input.pads()) {
        check(!pad.connected && pad.buttons == 0 && pad.left_x == 0.0F,
              "initial controller state is neutral");
    }
    input.poll(false);
    for (const auto& pad : input.pads()) {
        check(pad.buttons == 0 && pad.left_x == 0.0F && pad.left_y == 0.0F &&
              pad.right_x == 0.0F && pad.right_y == 0.0F &&
              pad.left_trigger == 0.0F && pad.right_trigger == 0.0F,
              "unfocused input is neutral regardless of physical controller");
    }
    check(!input.set_vibration(4, 0.0F, 0.0F), "invalid vibration slot rejected");
    bool rejected_nan = false;
    try { (void)input.set_vibration(0, std::numeric_limits<float>::quiet_NaN(), 0.0F); }
    catch (const std::invalid_argument&) { rejected_nan = true; }
    check(rejected_nan, "nonfinite vibration level rejected without touching hardware");
    input.stop_vibration();
    if (failures == 0) {
        std::cout << "PASS: normalization and neutral input regression checks\n";
    }
    return failures == 0 ? 0 : 1;
}
