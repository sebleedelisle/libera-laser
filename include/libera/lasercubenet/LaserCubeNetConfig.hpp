#pragma once

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
};

} // namespace libera::lasercubenet
