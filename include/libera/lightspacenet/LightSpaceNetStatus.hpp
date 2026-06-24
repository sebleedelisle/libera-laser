#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace libera::lightspacenet {

struct LightSpaceNetStatus {
    std::uint32_t deviceId = 0;
    std::uint16_t firmwareVersion = 0;
    std::uint16_t hardwareVersion = 0;
    std::string ipAddress;
    std::array<std::uint8_t, 6> macAddress{};
    std::string macAddressString;
    std::string deviceName;
    std::chrono::steady_clock::time_point lastSeen{};

    std::string stableId() const;
    std::string displayLabel() const;

    static std::optional<LightSpaceNetStatus> parseBroadcastResponse(
        const std::uint8_t* data,
        std::size_t size);
};

} // namespace libera::lightspacenet
