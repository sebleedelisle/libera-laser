#pragma once

#include "libera/core/BufferEstimator.hpp"

#include <chrono>
#include <cstdint>

namespace libera::lasercubenet {

struct LaserCubeNetConfig {
    static constexpr std::uint16_t ALIVE_PORT = 45456;
    static constexpr std::uint16_t COMMAND_PORT = 45457;
    static constexpr std::uint16_t DATA_PORT = 45458;

    static constexpr std::uint8_t CMD_GET_FULL_INFO = 0x77;
    static constexpr std::uint8_t CMD_ENABLE_BUFFER_RESPONSE = 0x78;
    static constexpr std::uint8_t CMD_SET_OUTPUT = 0x80;
    static constexpr std::uint8_t CMD_SET_ILDA_RATE = 0x82;
    static constexpr std::uint8_t CMD_GET_RINGBUFFER_FREE = 0x8a;
    static constexpr std::uint8_t CMD_SAMPLE_DATA = 0xa9;

    static constexpr std::size_t MAX_POINTS_PER_PACKET = 140; // fits within MTU
    static constexpr int SAFETY_HEADROOM_PACKETS = 2;
    static constexpr std::size_t SAFETY_HEADROOM_POINTS =
        MAX_POINTS_PER_PACKET * SAFETY_HEADROOM_PACKETS;

    static std::uint32_t clampPointRate(std::uint32_t pointRate, std::uint32_t maxPointRate) {
        if (maxPointRate > 0 && pointRate > maxPointRate) {
            return maxPointRate;
        }
        return pointRate;
    }

    static int targetBufferPoints(std::uint32_t pointRate,
                                  int bufferCapacity,
                                  std::chrono::milliseconds targetLatency) {
        return core::BufferEstimator::targetBufferPoints(
            pointRate,
            bufferCapacity,
            targetLatency,
            static_cast<int>(MAX_POINTS_PER_PACKET),
            static_cast<int>(SAFETY_HEADROOM_POINTS));
    }
};

} // namespace libera::lasercubenet
