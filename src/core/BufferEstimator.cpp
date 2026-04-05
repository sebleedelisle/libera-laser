#include "libera/core/BufferEstimator.hpp"

#include <algorithm>
#include <cmath>

namespace libera::core {

BufferEstimate BufferEstimator::estimateFromSnapshot(
    int snapshotBufferFullness,
    std::chrono::steady_clock::time_point snapshotTime,
    std::uint32_t pointRate,
    std::chrono::steady_clock::time_point now) {
    if (pointRate == 0) {
        return BufferEstimate{snapshotBufferFullness, false};
    }

    if (snapshotTime == std::chrono::steady_clock::time_point{}) {
        return BufferEstimate{snapshotBufferFullness, false};
    }

    const auto elapsed = now - snapshotTime;
    if (elapsed <= std::chrono::steady_clock::duration::zero()) {
        return BufferEstimate{snapshotBufferFullness, false};
    }

    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (elapsedUs <= 0) {
        return BufferEstimate{snapshotBufferFullness, false};
    }

    const double consumed =
        (static_cast<double>(pointRate) * static_cast<double>(elapsedUs)) / 1'000'000.0;
    const int estimated =
        static_cast<int>(std::llround(static_cast<double>(snapshotBufferFullness) - consumed));

    return BufferEstimate{std::max(0, estimated), true};
}

int BufferEstimator::minimumBufferPoints(
    std::uint32_t pointRate,
    std::chrono::milliseconds minimumBufferTime,
    int minimumBufferFloor) {
    const double fromTime =
        (static_cast<double>(pointRate) * static_cast<double>(minimumBufferTime.count())) / 1000.0;
    const int pointsFromTime = static_cast<int>(std::llround(fromTime));
    return std::max(pointsFromTime, minimumBufferFloor);
}

int BufferEstimator::targetBufferPoints(
    std::uint32_t pointRate,
    int bufferCapacity,
    std::chrono::milliseconds targetLatency,
    int minimumBufferFloor,
    int safetyHeadroomPoints) {
    if (bufferCapacity <= 0) {
        return 0;
    }

    const int minimumTarget = std::clamp(minimumBufferFloor, 0, bufferCapacity);
    if (pointRate == 0) {
        return minimumTarget;
    }

    const auto latencyMs = std::max<std::int64_t>(0, targetLatency.count());
    const double requestedFromTime =
        (static_cast<double>(pointRate) * static_cast<double>(latencyMs)) / 1000.0;
    const int requested = static_cast<int>(std::llround(requestedFromTime));

    const int maxSafetyHeadroom = std::max(0, bufferCapacity - minimumTarget);
    const int safetyHeadroom = std::clamp(safetyHeadroomPoints, 0, maxSafetyHeadroom);
    const int maximumTarget = std::max(minimumTarget, bufferCapacity - safetyHeadroom);

    return std::clamp(requested, minimumTarget, maximumTarget);
}

int BufferEstimator::clampSleepMillis(
    int millis,
    int minimumSleepMillis,
    int maximumSleepMillis) {
    if (maximumSleepMillis < minimumSleepMillis) {
        std::swap(maximumSleepMillis, minimumSleepMillis);
    }
    return std::clamp(millis, minimumSleepMillis, maximumSleepMillis);
}

} // namespace libera::core
