#pragma once

#include <chrono>
#include <cmath>
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
    static constexpr std::chrono::milliseconds TARGET_BUFFER_MS{10};

    static int targetBufferPoints(std::uint32_t pointRate, int bufferCapacity) {
        if (bufferCapacity <= 0) {
            return 0;
        }

        const int minPacketPoints = static_cast<int>(MAX_POINTS_PER_PACKET);
        const int minimumTarget =
            bufferCapacity < minPacketPoints ? bufferCapacity : minPacketPoints;
        if (pointRate == 0) {
            return minimumTarget;
        }

        const double requestedFromTime =
            (static_cast<double>(pointRate) *
             static_cast<double>(TARGET_BUFFER_MS.count())) / 1000.0;
        const int requested = static_cast<int>(std::lround(requestedFromTime));
        if (requested < minimumTarget) {
            return minimumTarget;
        }
        if (requested > bufferCapacity) {
            return bufferCapacity;
        }
        return requested;
    }
};

} // namespace libera::lasercubenet
