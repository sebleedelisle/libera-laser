#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <chrono>

namespace libera::lasercubenet {

struct LaserCubeNetStatus {
    std::uint8_t payloadVersion = 0;
    std::uint8_t firmwareMajor = 0;
    std::uint8_t firmwareMinor = 0;
    std::string firmwareVersion;

    bool outputEnabled = false;
    bool interlockEnabled = false;
    bool temperatureWarning = false;
    bool overTemperature = false;
    std::uint8_t packetErrors = 0;

    std::uint32_t pointRate = 0;
    std::uint32_t pointRateMax = 0;
    std::uint16_t bufferFree = 0;
    std::uint16_t bufferMax = 0;
    std::uint8_t batteryPercent = 0;
    std::uint8_t temperatureC = 0;
    std::uint8_t connectionType = 0;

    std::string serialNumber;
    std::string ipAddress;
    std::uint8_t modelNumber = 0;
    std::string modelName;

    std::chrono::steady_clock::time_point lastSeen{};

    static std::optional<LaserCubeNetStatus> parse(const std::uint8_t* data, std::size_t size);
};

} // namespace libera::lasercubenet
