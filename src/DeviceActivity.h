#pragma once
#include <Windows.h>
#include <Xinput.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

enum class InputSource : std::uint64_t { None = 0, Keyboard = 1, Mouse = 2, Gamepad = 3 };

inline bool IsRawMouseActivity(const RAWMOUSE& mouse) {
    constexpr USHORT pressOrWheel = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
        RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN |
        RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL;
    return (mouse.usButtonFlags & pressOrWheel) != 0 ||
        (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && (mouse.lLastX || mouse.lLastY));
}

// Producers only report real activity. The game thread consumes the newest
// token; controller connection, idle polling and releases are not activity.
class DeviceActivity {
public:
    void Notify(InputSource source, std::uint64_t now) {
        if (source == InputSource::None) return;
        const auto token = ((now ? now : 1) << 2) | static_cast<std::uint64_t>(source);
        auto previous = latest_.load(std::memory_order_relaxed);
        // The last observed source wins even within one clock tick. Comparing
        // the entire token would give Gamepad priority over Mouse/Keyboard.
        while ((token >> 2) >= (previous >> 2) && !latest_.compare_exchange_weak(previous, token,
            std::memory_order_release, std::memory_order_relaxed)) {}
    }
    std::uint64_t Peek(std::uint64_t now) const {
        const auto token = latest_.load(std::memory_order_acquire);
        const auto time = token >> 2;
        return time && now >= time && now - time <= 1000 ? token : 0;
    }
    void Clear() { latest_.store(0, std::memory_order_release); }
    static InputSource Source(std::uint64_t token) { return static_cast<InputSource>(token & 3); }
    static int NativeMode(std::uint64_t token) { return Source(token) == InputSource::Gamepad ? 3 : 2; }
private:
    std::atomic<std::uint64_t> latest_{ 0 };
};

// Called under the plugin's existing input mutex, including its low-rate poll.
class GamepadActivity {
public:
    bool Sample(unsigned index, const XINPUT_GAMEPAD& pad) {
        if (index >= pads_.size()) return false;
        auto& sample = pads_[index];
        if (!sample.connected) { sample = { true, pad, pad }; return false; }
        const auto& old = sample.pad;
        auto& anchor = sample.anchor;
        // Evaluate every analog control independently so a button edge cannot
        // skip its baseline update. Small deliberate movements accumulate from
        // the last activity, rather than being discarded at every hardware poll.
        const bool leftTrigger = Trigger(pad.bLeftTrigger, old.bLeftTrigger, anchor.bLeftTrigger);
        const bool rightTrigger = Trigger(pad.bRightTrigger, old.bRightTrigger, anchor.bRightTrigger);
        const bool leftStick = Stick(pad.sThumbLX, pad.sThumbLY, old.sThumbLX, old.sThumbLY,
            anchor.sThumbLX, anchor.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        const bool rightStick = Stick(pad.sThumbRX, pad.sThumbRY, old.sThumbRX, old.sThumbRY,
            anchor.sThumbRX, anchor.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        const bool active = (pad.wButtons & ~old.wButtons) != 0 ||
            leftTrigger || rightTrigger || leftStick || rightStick;
        sample.pad = pad;
        return active;
    }
    void Disconnect(unsigned index) { if (index < pads_.size()) pads_[index] = {}; }
private:
    struct SampleState { bool connected = false; XINPUT_GAMEPAD pad{}, anchor{}; };
    std::array<SampleState, 4> pads_{};
    static bool Trigger(BYTE value, BYTE old, BYTE& anchor) {
        if (value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD || value < old) {
            anchor = value;
            return false; // Releasing a trigger is not a device request.
        }
        const bool active = old <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD || int(value) - anchor >= 15;
        if (active) anchor = value;
        return active;
    }
    static bool TowardNeutral(int value, int old) {
        return std::abs(value) <= std::abs(old) &&
            (value == 0 || (value < 0) == (old < 0));
    }
    static bool Stick(SHORT x, SHORT y, SHORT oldX, SHORT oldY,
        SHORT& anchorX, SHORT& anchorY, int deadzone) {
        const auto squared = [](int a, int b) { return std::int64_t(a) * a + std::int64_t(b) * b; };
        const auto deadzoneSquared = std::int64_t(deadzone) * deadzone;
        if (squared(x, y) <= deadzoneSquared ||
            ((x != oldX || y != oldY) && TowardNeutral(x, oldX) && TowardNeutral(y, oldY))) {
            anchorX = x; anchorY = y;
            return false;
        }
        const bool active = squared(oldX, oldY) <= deadzoneSquared ||
            std::abs(int(x) - anchorX) >= 1500 || std::abs(int(y) - anchorY) >= 1500;
        if (active) { anchorX = x; anchorY = y; }
        return active;
    }
};
