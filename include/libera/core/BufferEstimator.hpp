#pragma once

#include <chrono>
#include <cstdint>

namespace libera::core {

struct BufferEstimate {
    int bufferFullness = 0;
    bool projected = false;
};

class BufferEstimator {
public:
    static BufferEstimate estimateFromAnchor(
        int anchorBufferFullness,
        std::chrono::steady_clock::time_point anchorTime,
        std::uint32_t pointRate,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    static int minimumBufferPoints(
        std::uint32_t pointRate,
        std::chrono::milliseconds minimumBufferTime,
        int minimumBufferFloor);

    static int clampSleepMillis(
        int millis,
        int minimumSleepMillis,
        int maximumSleepMillis);
};

} // namespace libera::core
