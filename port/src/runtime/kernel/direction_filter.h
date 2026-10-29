#pragma once
#include <cstdint>

namespace input_direction {
// Legacy preserves the original input. Last-input ties resolve to neutral.
enum Mode { Legacy, Neutral, UpPriority, LastInput };
// Axis deadzone preserves the original range outside the threshold. Zero means
// no override. D-pad-only changes sticks, never digital menu navigation.
inline int16_t Stick(int16_t value, bool dpadOnly, int deadzonePercent) {
    const int magnitude = value < 0 ? -int(value) : int(value);
    return dpadOnly || magnitude <= deadzonePercent * 32767 / 100 ? 0 : value;
}
// Reject a weak secondary stick axis relative to the dominant axis. This is
// angular diagonal tuning, separate from the absolute drift deadzone. Zero
// preserves the original shape; exact boundary values remain diagonal.
inline void TuneDiagonal(int16_t& x, int16_t& y, int percent) {
    if (percent <= 0) return;
    if (percent > 100) percent = 100;
    const int ax = x < 0 ? -int(x) : int(x);
    const int ay = y < 0 ? -int(y) : int(y);
    if (ax * 100 < ay * percent) x = 0;
    if (ay * 100 < ax * percent) y = 0;
}
struct Filter {
    uint16_t previous = 0;
    uint16_t winnerX = 0, winnerY = 0;
    uint16_t Apply(uint16_t buttons, int mode) {
        const uint16_t raw = buttons & 15;
        const uint16_t pressed = raw & ~previous;
        auto axis = [&](uint16_t mask, uint16_t& winner) {
            const uint16_t held = raw & mask, edge = pressed & mask;
            if (held != mask) winner = held;
            else if (edge) winner = edge == mask ? 0 : edge;
            if (held == mask) {
                buttons &= ~mask;
                if (mode == LastInput) buttons |= winner;
                else if (mode == UpPriority && mask == 3) buttons |= 1;
            }
        };
        if (mode != Legacy) { axis(12, winnerX); axis(3, winnerY); }
        previous = raw;
        return buttons;
    }
};
}
