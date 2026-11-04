#pragma once
#include <cmath>

inline float ApplySensitivityFactor(float original, float factor, int inputMode, bool savingSettings) {
    // CN 7.2 native getter and serializer both compare the joypad mode to 3.
    // The duplicate zero values in the managed enum dump are not authoritative.
    if (inputMode != 3 || savingSettings || factor == 1.0f) return original;
    const float result = original * factor;
    return std::isfinite(result) ? result : original;
}

inline bool ValidSensitivityFactor(float value) {
    return std::isfinite(value) && value > 0.0f;
}
