#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace libera::core {

// A single point to be sent to a laser controller.
// - x, y : normalised coordinates (-1..1)
// - r, g, b : colour channels (0..1 suggested)
// - u1, u2 : user fields for extension (waveforms, safety masks, etc.)
// - i : legacy master intensity for controllers that still require it

struct LaserPoint {
    float x = 0.0f;
    float y = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float i = 1.0f;
    float u1 = 0.0f;
    float u2 = 0.0f;


};

inline float sanitizeSignedUnitValue(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

inline float sanitizeUnitValue(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

inline float sanitizeFiniteValue(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return value;
}

inline void sanitizeLaserPoint(LaserPoint& point) noexcept {
    const bool invalidPosition =
        (!std::isfinite(point.x)) || (!std::isfinite(point.y));
    if (invalidPosition) {
        // A non-finite scan position cannot be safely clamped. Move to centre
        // and blank all output channels so bad geometry never reaches hardware.
        point.x = 0.0f;
        point.y = 0.0f;
        point.r = 0.0f;
        point.g = 0.0f;
        point.b = 0.0f;
        point.i = 0.0f;
        point.u1 = 0.0f;
        point.u2 = 0.0f;
        return;
    }

    // Keep finite out-of-range values unchanged at core ingress. Some callers
    // intentionally use wider values for scheduling/tests, and DAC encoders
    // still clamp to their hardware ranges at the final output boundary.
    point.r = sanitizeFiniteValue(point.r);
    point.g = sanitizeFiniteValue(point.g);
    point.b = sanitizeFiniteValue(point.b);
    point.i = sanitizeFiniteValue(point.i);
    point.u1 = sanitizeFiniteValue(point.u1);
    point.u2 = sanitizeFiniteValue(point.u2);
}

inline void sanitizeLaserPoints(std::vector<LaserPoint>& points) noexcept {
    for (auto& point : points) {
        sanitizeLaserPoint(point);
    }
}

} // namespace libera::core
