#include "reborn/input.hpp"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace reborn {

float normalize_axis(std::int16_t value, int deadzone) {
    if (deadzone < 0 || deadzone >= 32767) {
        throw std::invalid_argument("Axis dead zone must be between 0 and 32766");
    }
    const int sample = static_cast<int>(value);
    const int magnitude = sample < 0 ? -sample : sample;
    if (magnitude <= deadzone) {
        return 0.0F;
    }
    const int endpoint = sample < 0 ? 32768 : 32767;
    const float normalized = static_cast<float>(magnitude - deadzone) /
                             static_cast<float>(endpoint - deadzone);
    return sample < 0 ? -normalized : normalized;
}

float normalize_trigger(std::uint8_t value, std::uint8_t deadzone) {
    if (value <= deadzone) {
        return 0.0F;
    }
    return static_cast<float>(static_cast<int>(value) - static_cast<int>(deadzone)) /
           static_cast<float>(255 - static_cast<int>(deadzone));
}

InputSystem::~InputSystem() { stop_vibration(); }

void InputSystem::poll(bool focused) {
    if (!focused) {
        stop_vibration();
    }
    for (std::size_t slot = 0; slot < pads_.size(); ++slot) {
        XINPUT_STATE state{};
        PadState next{};
        if (XInputGetState(static_cast<DWORD>(slot), &state) == ERROR_SUCCESS) {
            next.connected = true;
            if (focused) {
                next.buttons = state.Gamepad.wButtons;
                next.left_x = normalize_axis(state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                next.left_y = normalize_axis(state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                next.right_x = normalize_axis(state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                next.right_y = normalize_axis(state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                next.left_trigger = normalize_trigger(state.Gamepad.bLeftTrigger);
                next.right_trigger = normalize_trigger(state.Gamepad.bRightTrigger);
            }
        } else {
            // Clear every sampled field on disconnect; stale held inputs must
            // never survive a missing controller. Reconnection is polled next frame.
            owns_vibration_[slot] = false;
        }
        pads_[slot] = next;
    }
}

std::array<PadState, 4> InputSystem::pads() const { return pads_; }

bool InputSystem::set_vibration(std::size_t slot, float left, float right) {
    if (!std::isfinite(left) || !std::isfinite(right)) {
        throw std::invalid_argument("Vibration levels must be finite");
    }
    if (slot >= pads_.size()) {
        return false;
    }
    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = static_cast<WORD>(
        std::lround(std::clamp(left, 0.0F, 1.0F) * 65535.0F));
    vibration.wRightMotorSpeed = static_cast<WORD>(
        std::lround(std::clamp(right, 0.0F, 1.0F) * 65535.0F));
    if (XInputSetState(static_cast<DWORD>(slot), &vibration) != ERROR_SUCCESS) {
        return false;
    }
    owns_vibration_[slot] = (vibration.wLeftMotorSpeed != 0 || vibration.wRightMotorSpeed != 0);
    return true;
}

void InputSystem::stop_vibration() noexcept {
    const XINPUT_VIBRATION stopped{};
    for (std::size_t slot = 0; slot < owns_vibration_.size(); ++slot) {
        if (owns_vibration_[slot]) {
            // Only stop effects started by this instance, avoiding writes to
            // controllers while other foreground applications own them.
            auto state = stopped;
            const DWORD result = XInputSetState(static_cast<DWORD>(slot), &state);
            if (result == ERROR_SUCCESS || result == ERROR_DEVICE_NOT_CONNECTED) {
                owns_vibration_[slot] = false;
            }
        }
    }
}

} // namespace reborn
