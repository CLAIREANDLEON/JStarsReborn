#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace reborn {

struct PadState {
    bool connected = false;
    std::uint16_t buttons = 0;
    float left_x = 0.0F;
    float left_y = 0.0F;
    float right_x = 0.0F;
    float right_y = 0.0F;
    float left_trigger = 0.0F;
    float right_trigger = 0.0F;
};

// Per-axis normalization, with distinct negative/positive integer endpoints.
// Dead zone must be in 0..32766; output is in [-1, 1].
[[nodiscard]] float normalize_axis(std::int16_t value, int deadzone);
// A dead zone of 255 disables the entire trigger; output is in [0, 1].
[[nodiscard]] float normalize_trigger(std::uint8_t value, std::uint8_t deadzone = 30);

// Poll on the application thread. Supports XInput-compatible devices only;
// native DualShock 4/DualSense HID support is a later milestone.
class InputSystem final {
public:
    InputSystem() = default;
    ~InputSystem();
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    void poll(bool focused);
    [[nodiscard]] std::array<PadState, 4> pads() const;
    // Opt-in only: the runtime never starts vibration while merely polling.
    // Returns false for an absent device or invalid slot. Levels are clamped
    // to [0, 1]; non-finite levels throw std::invalid_argument.
    [[nodiscard]] bool set_vibration(std::size_t slot, float left, float right);
    void stop_vibration() noexcept;

private:
    std::array<PadState, 4> pads_{};
    std::array<bool, 4> owns_vibration_{};
};

} // namespace reborn
