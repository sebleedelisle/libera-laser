#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>

namespace libera::lightspacenet {

struct LightSpaceNetConfig {
    static constexpr std::uint16_t NETWORK_PORT = 25555;
    static constexpr std::uint16_t PROTOCOL_VERSION = 0x0001;

    static constexpr std::uint8_t PACKET_TYPE_BASIC = 0x01;
    static constexpr std::uint8_t PACKET_TYPE_COMMAND = 0x02;

    static constexpr std::uint8_t CMD_BROADCAST_QUERY = 0x01;
    static constexpr std::uint8_t CMD_BROADCAST_RESPONSE = 0x02;
    static constexpr std::uint8_t CMD_HEARTBEAT_QUERY = 0x03;
    static constexpr std::uint8_t CMD_HEARTBEAT_RESPONSE = 0x04;
    static constexpr std::uint8_t CMD_POINT_STREAM = 0x10;

    static constexpr std::uint8_t CMD_COMMAND_ACK = 0x01;
    static constexpr std::uint8_t CMD_LASER_ON_OFF = 0x02;
    static constexpr std::uint8_t CMD_SET_SCAN_FREQUENCY = 0x03;

    static constexpr std::uint32_t MIN_POINT_RATE = 1000;
    static constexpr std::uint32_t DEFAULT_POINT_RATE = 30000;
    static constexpr std::uint32_t MAX_POINT_RATE = 100000;

    // The LS-Net point packet describes the "current pattern", so normal
    // playback should send one complete source frame per upload. Hardware
    // probing showed the current firmware accepts a single 5118-byte pattern
    // packet and stops answering at 5125 bytes, consistent with a 5120-byte
    // packet cap. Repeated playback near that boundary is unreliable, so the
    // operational cap leaves substantial margin below the measured one-shot
    // limit. This is a hard transport cap, not a target size: oversized frames
    // are fitted by preserving source prefix points and appending blank travel
    // to the original frame end, not by resampling the whole frame.
    static constexpr std::size_t MAX_CURRENT_PATTERN_PACKET_BYTES = 5120;
    static constexpr std::size_t CURRENT_PATTERN_PACKET_OVERHEAD = 22;
    static constexpr std::size_t BYTES_PER_POINT = 7;
    static constexpr std::size_t MEASURED_MAX_SOURCE_FRAME_POINTS =
        (MAX_CURRENT_PATTERN_PACKET_BYTES - CURRENT_PATTERN_PACKET_OVERHEAD) /
        BYTES_PER_POINT;
    static constexpr std::size_t MAX_SOURCE_FRAME_POINTS = 700;
    static constexpr std::size_t DEFAULT_PATTERN_POINTS = MAX_SOURCE_FRAME_POINTS;
    static constexpr auto DEFAULT_PATTERN_UPDATE_INTERVAL = std::chrono::milliseconds(40);

    static constexpr auto COMMAND_ACK_TIMEOUT = std::chrono::milliseconds(100);
    static constexpr int COMMAND_ACK_ATTEMPTS = 3;
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(1);
    static constexpr auto HEARTBEAT_DISCONNECT_AFTER = std::chrono::seconds(3);
    static constexpr auto INCOMING_POLL_INTERVAL = std::chrono::milliseconds(20);

    // Discovery uses short sessions so a background scanner does not hold the
    // protocol port continuously when the application is only listing devices.
    static constexpr auto DISCOVERY_RECV_TIMEOUT = std::chrono::milliseconds(100);
    static constexpr auto DISCOVERY_PROBE_INTERVAL = std::chrono::milliseconds(300);
    static constexpr auto DISCOVERY_LISTEN_WINDOW = std::chrono::milliseconds(1200);
    static constexpr auto DISCOVERY_IDLE_INTERVAL = std::chrono::seconds(5);
    static constexpr auto DISCOVERY_STALE_AFTER = std::chrono::seconds(15);

    static std::uint32_t clampPointRate(std::uint32_t pointRate) {
        if (pointRate == 0) {
            return DEFAULT_POINT_RATE;
        }
        return std::clamp(pointRate, MIN_POINT_RATE, MAX_POINT_RATE);
    }

    static std::uint8_t scanFrequencyKilohertz(std::uint32_t pointRate) {
        const auto clamped = clampPointRate(pointRate);
        const auto rounded = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(clamped) / 1000.0));
        return static_cast<std::uint8_t>(
            std::clamp<std::uint32_t>(rounded, 1, 30));
    }
};

} // namespace libera::lightspacenet
