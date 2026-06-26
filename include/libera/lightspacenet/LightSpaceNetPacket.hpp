#pragma once

#include "libera/core/LaserPoint.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace libera::lightspacenet {

struct LightSpaceNetPacket {
    std::uint16_t version = 0;
    std::uint8_t packetType = 0;
    std::uint8_t commandWord = 0;
    std::vector<std::uint8_t> payload;
};

enum class LightSpaceNetCoordinateEncoding {
    Signed16,
    Unsigned16,
    Signed15,
    Unsigned15,
    Signed12,
    Unsigned12
};

enum class LightSpaceNetCoordinateByteOrder {
    BigEndian,
    LittleEndian
};

struct LightSpaceNetCoordinateOptions {
    LightSpaceNetCoordinateEncoding encoding = LightSpaceNetCoordinateEncoding::Signed16;
    LightSpaceNetCoordinateByteOrder byteOrder = LightSpaceNetCoordinateByteOrder::BigEndian;
    bool invertX = false;
    bool invertY = false;
    bool swapXY = false;
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

std::uint16_t crc16Ccitt(const std::uint8_t* data, std::size_t size) noexcept;

std::uint16_t readBe16(const std::uint8_t* data) noexcept;
std::uint32_t readBe32(const std::uint8_t* data) noexcept;
std::uint64_t readBe64(const std::uint8_t* data) noexcept;

void appendBe16(std::vector<std::uint8_t>& output, std::uint16_t value);
void appendBe32(std::vector<std::uint8_t>& output, std::uint32_t value);
void appendBe64(std::vector<std::uint8_t>& output, std::uint64_t value);

std::vector<std::uint8_t> buildPacket(std::uint8_t packetType,
                                      std::uint8_t commandWord,
                                      const std::uint8_t* payload,
                                      std::size_t payloadSize);
std::vector<std::uint8_t> buildPacket(std::uint8_t packetType,
                                      std::uint8_t commandWord,
                                      const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> buildBroadcastQueryPacket();
std::vector<std::uint8_t> buildHeartbeatQueryPacket(std::uint64_t timestampMillis);
std::vector<std::uint8_t> buildLaserSwitchPacket(bool laserOn);
std::vector<std::uint8_t> buildScanFrequencyPacket(std::uint32_t pointRate);
std::vector<std::uint8_t> buildPointStreamPacket(const std::vector<core::LaserPoint>& points);
std::vector<std::uint8_t> buildPointStreamPacket(
    const std::vector<core::LaserPoint>& points,
    const LightSpaceNetCoordinateOptions& coordinateOptions);

std::vector<core::LaserPoint> fitCurrentPatternToPointLimit(
    const std::vector<core::LaserPoint>& points,
    std::size_t maximumPointCount);

std::optional<LightSpaceNetPacket> parsePacket(const std::uint8_t* data,
                                               std::size_t size);

} // namespace libera::lightspacenet
