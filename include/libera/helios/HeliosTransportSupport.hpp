#pragma once

#include "libera/core/LaserPoint.hpp"
#include "HeliosDac.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace libera::helios::detail {

inline constexpr std::size_t MIN_FRAME_POINTS = 20;
inline constexpr double TARGET_FRAME_DURATION_MS = 10.0;
inline constexpr auto STATUS_ERROR_WARMUP_GRACE = std::chrono::milliseconds(250);
inline constexpr unsigned int HELIOS_FLAGS = HELIOS_FLAGS_DEFAULT;

inline bool shouldLogErrorBurst(std::size_t consecutiveCount) {
    // Always log the first failure, then throttle repeated errors so unstable
    // hardware does not bury more useful diagnostics in the console.
    return consecutiveCount == 1 || (consecutiveCount % 25 == 0);
}

inline std::size_t defaultFramePointCount(std::uint32_t pointRate) {
    if (pointRate == 0) {
        return MIN_FRAME_POINTS;
    }

    const double rawPoints =
        (static_cast<double>(pointRate) * TARGET_FRAME_DURATION_MS) / 1000.0;
    const auto roundedPoints = static_cast<std::size_t>(std::llround(rawPoints));
    return std::clamp<std::size_t>(roundedPoints, MIN_FRAME_POINTS, HELIOS_MAX_POINTS);
}

inline std::size_t minimumRequestPoints(std::size_t maxFramePoints) {
    return std::min<std::size_t>(maxFramePoints, MIN_FRAME_POINTS);
}

inline std::uint16_t encodeUnsigned16FromSignedUnitValue(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float normalized = (clamped * 0.5f) + 0.5f;
    return static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
}

inline std::uint16_t encodeUnsigned16FromUnitValue(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(clamped * 65535.0f));
}

inline std::chrono::steady_clock::duration requestRenderLead(
    std::chrono::microseconds previousWriteLead) {
    return std::chrono::microseconds(std::max<std::int64_t>(0, previousWriteLead.count()));
}

inline std::int64_t smoothWriteLeadMicros(std::int64_t previousMicros,
                                          std::int64_t currentMicros) {
    const auto clampedPrevious = std::max<std::int64_t>(0, previousMicros);
    const auto clampedCurrent = std::max<std::int64_t>(0, currentMicros);
    if (clampedPrevious == 0) {
        return clampedCurrent;
    }
    return ((clampedPrevious * 3) + clampedCurrent) / 4;
}

inline void encodeFramePoints(const std::vector<core::LaserPoint>& inputPoints,
                              std::vector<HeliosPointExt>& outputPoints) {
    outputPoints.resize(inputPoints.size());
    for (std::size_t i = 0; i < inputPoints.size(); ++i) {
        const auto& point = inputPoints[i];
        auto& encoded = outputPoints[i];
        encoded.x = encodeUnsigned16FromSignedUnitValue(point.x);
        encoded.y = encodeUnsigned16FromSignedUnitValue(point.y);
        encoded.r = encodeUnsigned16FromUnitValue(point.r);
        encoded.g = encodeUnsigned16FromUnitValue(point.g);
        encoded.b = encodeUnsigned16FromUnitValue(point.b);
        encoded.i = encodeUnsigned16FromUnitValue(point.i);
        encoded.user1 = encodeUnsigned16FromUnitValue(point.u1);
        encoded.user2 = encodeUnsigned16FromUnitValue(point.u2);
        encoded.user3 = 0;
        encoded.user4 = 0;
    }
}

} // namespace libera::helios::detail
