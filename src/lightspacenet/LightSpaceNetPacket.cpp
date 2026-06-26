#include "libera/lightspacenet/LightSpaceNetPacket.hpp"

#include "libera/lightspacenet/LightSpaceNetConfig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace libera::lightspacenet {

namespace {

constexpr std::array<std::uint8_t, 10> protocolHeader{
    'L', 'I', 'G', 'H', 'T', 'S', 'P', 'A', 'C', 'E'
};
constexpr float fullCoordinateWidth = 2.0f;
constexpr float blankTravelSpacing = fullCoordinateWidth * 0.02f;
constexpr std::size_t minBlankTravelPointCount = 2;
constexpr std::size_t maxBlankTravelPointCount = 128;

std::int16_t encodeSigned16FromSignedUnit(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    if (clamped <= -1.0f) {
        return static_cast<std::int16_t>(-32768);
    }
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
}

std::int16_t encodeSignedRangeFromSignedUnit(float value, int negativeFullScale, int positiveFullScale) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    if (clamped <= -1.0f) {
        return static_cast<std::int16_t>(negativeFullScale);
    }
    return static_cast<std::int16_t>(
        std::lround(clamped * static_cast<float>(positiveFullScale)));
}

std::uint16_t encodeUnsigned16FromSignedUnit(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float normalized = (clamped * 0.5f) + 0.5f;
    return static_cast<std::uint16_t>(std::lround(normalized * 65535.0f));
}

std::uint16_t encodeUnsignedRangeFromSignedUnit(float value, int maxValue) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    const float normalized = (clamped * 0.5f) + 0.5f;
    return static_cast<std::uint16_t>(
        std::lround(normalized * static_cast<float>(maxValue)));
}

std::uint8_t encodeUnsigned8FromUnit(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

float cubicEaseInOut(float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    if (clamped < 0.5f) {
        return 4.0f * clamped * clamped * clamped;
    }

    const float inv = -2.0f * clamped + 2.0f;
    return 1.0f - ((inv * inv * inv) / 2.0f);
}

std::size_t blankTravelPointCount(const core::LaserPoint& from,
                                  const core::LaserPoint& to) {
    const float fromX = core::sanitizeSignedUnitValue(from.x);
    const float fromY = core::sanitizeSignedUnitValue(from.y);
    const float toX = core::sanitizeSignedUnitValue(to.x);
    const float toY = core::sanitizeSignedUnitValue(to.y);
    const float dx = toX - fromX;
    const float dy = toY - fromY;
    const float distance = std::sqrt(dx * dx + dy * dy);

    // LaserPoint X/Y are signed unit coordinates, so the full coordinate width
    // is -1..+1. Keep the blank move quick: one segment per 2% of that width.
    const auto travelSegments = static_cast<std::size_t>(
        std::ceil(distance / blankTravelSpacing));
    return std::min(maxBlankTravelPointCount,
                    std::max<std::size_t>(travelSegments + 1,
                                          minBlankTravelPointCount));
}

void appendBlankTravelPoints(std::vector<core::LaserPoint>& output,
                             const core::LaserPoint& from,
                             const core::LaserPoint& to,
                             std::size_t pointCount) {
    if (pointCount == 0) {
        return;
    }

    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    for (std::size_t i = 0; i < pointCount; ++i) {
        const float t = static_cast<float>(i) /
                        static_cast<float>(std::max<std::size_t>(pointCount - 1, 1));
        const float eased = cubicEaseInOut(t);

        core::LaserPoint blankPoint = from;
        blankPoint.x = from.x + (dx * eased);
        blankPoint.y = from.y + (dy * eased);
        blankPoint.r = 0.0f;
        blankPoint.g = 0.0f;
        blankPoint.b = 0.0f;
        blankPoint.i = 0.0f;
        output.push_back(blankPoint);
    }
}

void appendCoordinateWord(std::vector<std::uint8_t>& payload,
                          std::uint16_t value,
                          LightSpaceNetCoordinateByteOrder byteOrder) {
    if (byteOrder == LightSpaceNetCoordinateByteOrder::LittleEndian) {
        payload.push_back(static_cast<std::uint8_t>(value & 0xFFu));
        payload.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    } else {
        appendBe16(payload, value);
    }
}

void appendCoordinate(std::vector<std::uint8_t>& payload,
                      float value,
                      const LightSpaceNetCoordinateOptions& options) {
    std::uint16_t encoded = 0;
    switch (options.encoding) {
    case LightSpaceNetCoordinateEncoding::Unsigned16:
        encoded = encodeUnsigned16FromSignedUnit(value);
        break;
    case LightSpaceNetCoordinateEncoding::Signed15:
        encoded = static_cast<std::uint16_t>(
            encodeSignedRangeFromSignedUnit(value, -16384, 16383));
        break;
    case LightSpaceNetCoordinateEncoding::Unsigned15:
        encoded = encodeUnsignedRangeFromSignedUnit(value, 32767);
        break;
    case LightSpaceNetCoordinateEncoding::Signed12:
        encoded = static_cast<std::uint16_t>(
            encodeSignedRangeFromSignedUnit(value, -2048, 2047));
        break;
    case LightSpaceNetCoordinateEncoding::Unsigned12:
        encoded = encodeUnsignedRangeFromSignedUnit(value, 4095);
        break;
    case LightSpaceNetCoordinateEncoding::Signed16:
    default:
        // Signed mode is an implementation assumption: the LS-Net docs only
        // say X/Y are two-byte fields, not the numeric coordinate range.
        encoded = static_cast<std::uint16_t>(encodeSigned16FromSignedUnit(value));
        break;
    }

    appendCoordinateWord(payload, encoded, options.byteOrder);
}

} // namespace

std::uint16_t crc16Ccitt(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint16_t crc = 0xFFFF;
    if (!data && size > 0) {
        return crc;
    }

    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000u) != 0) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021u);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }

    return crc;
}

std::uint16_t readBe16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t readBe32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t readBe64(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint64_t>(readBe32(data)) << 32) |
           static_cast<std::uint64_t>(readBe32(data + 4));
}

void appendBe16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    output.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void appendBe32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    output.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void appendBe64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    appendBe32(output, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFu));
    appendBe32(output, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

std::vector<std::uint8_t> buildPacket(std::uint8_t packetType,
                                      std::uint8_t commandWord,
                                      const std::uint8_t* payload,
                                      std::size_t payloadSize) {
    constexpr std::size_t fixedPacketOverhead = 20;
    const auto packetSize = fixedPacketOverhead + payloadSize;
    if (packetSize > 0xFFFFu || payloadSize > 0xFFFFu) {
        return {};
    }

    std::vector<std::uint8_t> output;
    output.reserve(packetSize);
    output.insert(output.end(), protocolHeader.begin(), protocolHeader.end());
    appendBe16(output, static_cast<std::uint16_t>(packetSize));
    appendBe16(output, LightSpaceNetConfig::PROTOCOL_VERSION);
    output.push_back(packetType);
    output.push_back(commandWord);
    appendBe16(output, static_cast<std::uint16_t>(payloadSize));
    if (payload && payloadSize > 0) {
        output.insert(output.end(), payload, payload + payloadSize);
    }

    // The protocol CRC covers every field before the CRC itself.
    const auto crc = crc16Ccitt(output.data(), output.size());
    appendBe16(output, crc);
    return output;
}

std::vector<std::uint8_t> buildPacket(std::uint8_t packetType,
                                      std::uint8_t commandWord,
                                      const std::vector<std::uint8_t>& payload) {
    return buildPacket(packetType, commandWord, payload.data(), payload.size());
}

std::vector<std::uint8_t> buildBroadcastQueryPacket() {
    const std::uint8_t payload = 0xFF;
    return buildPacket(LightSpaceNetConfig::PACKET_TYPE_BASIC,
                       LightSpaceNetConfig::CMD_BROADCAST_QUERY,
                       &payload,
                       1);
}

std::vector<std::uint8_t> buildHeartbeatQueryPacket(std::uint64_t timestampMillis) {
    std::vector<std::uint8_t> payload;
    appendBe64(payload, timestampMillis);
    return buildPacket(LightSpaceNetConfig::PACKET_TYPE_BASIC,
                       LightSpaceNetConfig::CMD_HEARTBEAT_QUERY,
                       payload);
}

std::vector<std::uint8_t> buildLaserSwitchPacket(bool laserOn) {
    const std::uint8_t payload = laserOn ? 0x01 : 0x02;
    return buildPacket(LightSpaceNetConfig::PACKET_TYPE_COMMAND,
                       LightSpaceNetConfig::CMD_LASER_ON_OFF,
                       &payload,
                       1);
}

std::vector<std::uint8_t> buildScanFrequencyPacket(std::uint32_t pointRate) {
    const std::uint8_t payload = LightSpaceNetConfig::scanFrequencyKilohertz(pointRate);
    return buildPacket(LightSpaceNetConfig::PACKET_TYPE_COMMAND,
                       LightSpaceNetConfig::CMD_SET_SCAN_FREQUENCY,
                       &payload,
                       1);
}

std::vector<std::uint8_t> buildPointStreamPacket(const std::vector<core::LaserPoint>& points) {
    LightSpaceNetCoordinateOptions coordinateOptions;
    return buildPointStreamPacket(points, coordinateOptions);
}

std::vector<std::uint8_t> buildPointStreamPacket(
    const std::vector<core::LaserPoint>& points,
    const LightSpaceNetCoordinateOptions& coordinateOptions) {
    const auto pointCount =
        static_cast<std::uint16_t>(std::min<std::size_t>(points.size(), 0xFFFFu));
    std::vector<std::uint8_t> payload;
    payload.reserve(2 + (static_cast<std::size_t>(pointCount) * 7));
    appendBe16(payload, pointCount);

    for (std::size_t i = 0; i < pointCount; ++i) {
        const auto& point = points[i];
        float x = point.x;
        float y = point.y;
        if (coordinateOptions.swapXY) {
            std::swap(x, y);
        }
        if (coordinateOptions.invertX) {
            x = -x;
        }
        if (coordinateOptions.invertY) {
            y = -y;
        }
        x = (x * coordinateOptions.scale) + coordinateOptions.offsetX;
        y = (y * coordinateOptions.scale) + coordinateOptions.offsetY;

        appendCoordinate(payload, x, coordinateOptions);
        appendCoordinate(payload, y, coordinateOptions);
        payload.push_back(encodeUnsigned8FromUnit(point.r));
        payload.push_back(encodeUnsigned8FromUnit(point.g));
        payload.push_back(encodeUnsigned8FromUnit(point.b));
    }

    return buildPacket(LightSpaceNetConfig::PACKET_TYPE_BASIC,
                       LightSpaceNetConfig::CMD_POINT_STREAM,
                       payload);
}

std::vector<core::LaserPoint> fitCurrentPatternToPointLimit(
    const std::vector<core::LaserPoint>& points,
    std::size_t maximumPointCount) {
    if (points.size() <= maximumPointCount) {
        return points;
    }
    if (points.empty() || maximumPointCount == 0) {
        return {};
    }
    if (maximumPointCount == 1) {
        core::LaserPoint endPoint = points.back();
        endPoint.r = 0.0f;
        endPoint.g = 0.0f;
        endPoint.b = 0.0f;
        endPoint.i = 0.0f;
        return {endPoint};
    }

    const auto& finalPoint = points.back();
    std::size_t blankTailCount = std::min<std::size_t>(
        blankTravelPointCount(points[maximumPointCount - 2], finalPoint),
        maximumPointCount - 1);

    for (int i = 0; i < 8; ++i) {
        const std::size_t visiblePointCount = maximumPointCount - blankTailCount;
        const auto& lastVisiblePoint = points[visiblePointCount - 1];
        const std::size_t desiredBlankTailCount = std::min<std::size_t>(
            blankTravelPointCount(lastVisiblePoint, finalPoint),
            maximumPointCount - 1);
        if (desiredBlankTailCount == blankTailCount) {
            break;
        }
        blankTailCount = desiredBlankTailCount;
    }

    const std::size_t visiblePointCount = maximumPointCount - blankTailCount;
    std::vector<core::LaserPoint> fitted;
    fitted.reserve(maximumPointCount);
    fitted.insert(fitted.end(),
                  points.begin(),
                  points.begin() + static_cast<std::ptrdiff_t>(visiblePointCount));
    appendBlankTravelPoints(fitted,
                            fitted.back(),
                            finalPoint,
                            blankTailCount);
    return fitted;
}

std::optional<LightSpaceNetPacket> parsePacket(const std::uint8_t* data,
                                               std::size_t size) {
    constexpr std::size_t minimumPacketSize = 20;
    if (!data || size < minimumPacketSize) {
        return std::nullopt;
    }
    if (std::memcmp(data, protocolHeader.data(), protocolHeader.size()) != 0) {
        return std::nullopt;
    }

    const auto packetSize = readBe16(data + 10);
    if (packetSize != size) {
        return std::nullopt;
    }

    const auto dataLength = readBe16(data + 16);
    if (static_cast<std::size_t>(dataLength) + minimumPacketSize != size) {
        return std::nullopt;
    }

    const auto expectedCrc = readBe16(data + size - 2);
    const auto actualCrc = crc16Ccitt(data, size - 2);
    if (expectedCrc != actualCrc) {
        return std::nullopt;
    }

    LightSpaceNetPacket packet;
    packet.version = readBe16(data + 12);
    packet.packetType = data[14];
    packet.commandWord = data[15];
    packet.payload.assign(data + 18, data + 18 + dataLength);
    return packet;
}

} // namespace libera::lightspacenet
