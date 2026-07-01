#include "libera/lightspacenet/LightSpaceNetConfig.hpp"
#include "libera/lightspacenet/LightSpaceNetPacket.hpp"
#include "libera/lightspacenet/LightSpaceNetStatus.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace libera;
using namespace libera::lightspacenet;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

void testBroadcastQueryMatchesDocumentExample() {
    const std::array<std::uint8_t, 21> expected{
        0x4C, 0x49, 0x47, 0x48, 0x54, 0x53, 0x50, 0x41, 0x43, 0x45,
        0x00, 0x15,
        0x00, 0x01,
        0x01, 0x01,
        0x00, 0x01,
        0xFF,
        0x87, 0xB2,
    };

    const auto packet = buildBroadcastQueryPacket();
    ASSERT_EQ(packet.size(), expected.size(), "broadcast query size");
    ASSERT_TRUE(std::equal(packet.begin(), packet.end(), expected.begin()),
                "broadcast query bytes should match protocol document");
    ASSERT_EQ(crc16Ccitt(packet.data(), packet.size() - 2),
              static_cast<std::uint16_t>(0x87B2),
              "broadcast query crc");
}

void testParsesBuiltPacket() {
    const auto packet = buildHeartbeatQueryPacket(0x0102030405060708ull);
    const auto parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "heartbeat packet parses");
    ASSERT_EQ(parsed->version, LightSpaceNetConfig::PROTOCOL_VERSION, "protocol version");
    ASSERT_EQ(parsed->packetType, LightSpaceNetConfig::PACKET_TYPE_BASIC, "packet type");
    ASSERT_EQ(parsed->commandWord, LightSpaceNetConfig::CMD_HEARTBEAT_QUERY, "command word");
    ASSERT_EQ(parsed->payload.size(), static_cast<std::size_t>(8), "heartbeat payload size");
    ASSERT_EQ(readBe64(parsed->payload.data()), 0x0102030405060708ull, "heartbeat timestamp");
}

void testRejectsBadCrc() {
    auto packet = buildBroadcastQueryPacket();
    packet.back() ^= 0x01;
    ASSERT_TRUE(!parsePacket(packet.data(), packet.size()).has_value(),
                "packet with bad crc is rejected");
}

void testPointStreamEncoding() {
    std::vector<core::LaserPoint> points;
    points.push_back(core::LaserPoint{-1.0f, 0.0f, 1.0f, 0.5f, 0.0f});
    points.push_back(core::LaserPoint{1.0f, -1.0f, 0.0f, 0.0f, 1.0f});

    const auto packet = buildPointStreamPacket(points);
    const auto parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "point stream parses");
    ASSERT_EQ(parsed->packetType, LightSpaceNetConfig::PACKET_TYPE_BASIC, "point stream type");
    ASSERT_EQ(parsed->commandWord, LightSpaceNetConfig::CMD_POINT_STREAM, "point stream command");
    ASSERT_EQ(readBe16(parsed->payload.data()), static_cast<std::uint16_t>(2), "point count");

    const auto* firstPoint = parsed->payload.data() + 2;
    ASSERT_EQ(readBe16(firstPoint + 0), static_cast<std::uint16_t>(0x8000), "x -1 maps to signed minimum");
    ASSERT_EQ(readBe16(firstPoint + 2), static_cast<std::uint16_t>(0), "y 0 maps to signed centre");
    ASSERT_EQ(firstPoint[4], static_cast<std::uint8_t>(255), "red maps to 255");
    ASSERT_EQ(firstPoint[5], static_cast<std::uint8_t>(128), "green half maps to 128");
    ASSERT_EQ(firstPoint[6], static_cast<std::uint8_t>(0), "blue maps to 0");
}

void testUnsignedPointStreamEncodingCanBeSelected() {
    std::vector<core::LaserPoint> points;
    points.push_back(core::LaserPoint{-1.0f, 0.0f, 1.0f, 0.0f, 0.0f});

    LightSpaceNetCoordinateOptions options;
    options.encoding = LightSpaceNetCoordinateEncoding::Unsigned16;

    const auto packet = buildPointStreamPacket(points, options);
    const auto parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "unsigned point stream parses");

    const auto* firstPoint = parsed->payload.data() + 2;
    ASSERT_EQ(readBe16(firstPoint + 0), static_cast<std::uint16_t>(0), "unsigned x -1 maps to 0");
    ASSERT_EQ(readBe16(firstPoint + 2), static_cast<std::uint16_t>(0x8000), "unsigned y 0 maps to centre");
}

void testCoordinateAxisOptions() {
    std::vector<core::LaserPoint> points;
    points.push_back(core::LaserPoint{0.25f, -0.5f, 1.0f, 0.0f, 0.0f});

    LightSpaceNetCoordinateOptions options;
    options.invertX = true;
    options.invertY = true;
    options.swapXY = true;

    const auto packet = buildPointStreamPacket(points, options);
    const auto parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "axis option point stream parses");

    const auto* firstPoint = parsed->payload.data() + 2;
    ASSERT_EQ(readBe16(firstPoint + 0), static_cast<std::uint16_t>(0x4000), "swapped/inverted x encodes original -y");
    ASSERT_EQ(readBe16(firstPoint + 2), static_cast<std::uint16_t>(0xE000), "swapped/inverted y encodes original -x");
}

void testNarrowCoordinateEncodingsCanBeSelected() {
    std::vector<core::LaserPoint> points;
    points.push_back(core::LaserPoint{-1.0f, 0.0f, 1.0f, 0.0f, 0.0f});

    LightSpaceNetCoordinateOptions options;
    options.encoding = LightSpaceNetCoordinateEncoding::Unsigned12;

    auto packet = buildPointStreamPacket(points, options);
    auto parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "unsigned12 point stream parses");
    const auto* firstPoint = parsed->payload.data() + 2;
    ASSERT_EQ(readBe16(firstPoint + 0), static_cast<std::uint16_t>(0), "unsigned12 x -1 maps to 0");
    ASSERT_EQ(readBe16(firstPoint + 2), static_cast<std::uint16_t>(2048), "unsigned12 y 0 maps to centre");

    options.encoding = LightSpaceNetCoordinateEncoding::Signed12;
    packet = buildPointStreamPacket(points, options);
    parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "signed12 point stream parses");
    firstPoint = parsed->payload.data() + 2;
    ASSERT_EQ(readBe16(firstPoint + 0), static_cast<std::uint16_t>(0xF800), "signed12 x -1 maps to -2048");
    ASSERT_EQ(readBe16(firstPoint + 2), static_cast<std::uint16_t>(0), "signed12 y 0 maps to centre");
}

void testCoordinateScaleAndByteOrderCanBeSelected() {
    std::vector<core::LaserPoint> points;
    points.push_back(core::LaserPoint{1.0f, 0.0f, 1.0f, 0.0f, 0.0f});

    LightSpaceNetCoordinateOptions options;
    options.scale = 0.5f;
    options.byteOrder = LightSpaceNetCoordinateByteOrder::LittleEndian;

    const auto packet = buildPointStreamPacket(points, options);
    const auto parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "little-endian scaled point stream parses");

    const auto* firstPoint = parsed->payload.data() + 2;
    ASSERT_EQ(firstPoint[0], static_cast<std::uint8_t>(0x00), "little-endian x low byte first");
    ASSERT_EQ(firstPoint[1], static_cast<std::uint8_t>(0x40), "little-endian x high byte second");
}

void testParsesBroadcastResponse() {
    std::vector<std::uint8_t> payload;
    appendBe32(payload, 0x01020304);
    appendBe16(payload, 0x0005);
    appendBe16(payload, 0x0006);
    payload.insert(payload.end(), {192, 168, 1, 77});
    payload.insert(payload.end(), {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
    const char name[] = "LS DAC";
    payload.insert(payload.end(), name, name + 6);

    const auto packet = buildPacket(LightSpaceNetConfig::PACKET_TYPE_BASIC,
                                    LightSpaceNetConfig::CMD_BROADCAST_RESPONSE,
                                    payload);
    const auto status = LightSpaceNetStatus::parseBroadcastResponse(packet.data(), packet.size());
    ASSERT_TRUE(status.has_value(), "broadcast response parses");
    ASSERT_EQ(status->deviceId, static_cast<std::uint32_t>(0x01020304), "device id");
    ASSERT_EQ(status->firmwareVersion, static_cast<std::uint16_t>(5), "firmware version");
    ASSERT_EQ(status->hardwareVersion, static_cast<std::uint16_t>(6), "hardware version");
    ASSERT_TRUE(status->ipAddress == "192.168.1.77", "ip address");
    ASSERT_TRUE(status->macAddressString == "AABBCCDDEEFF", "mac string");
    ASSERT_TRUE(status->stableId() == "AABBCCDDEEFF", "stable id");
    ASSERT_TRUE(status->displayLabel() == "LS DAC", "display label");
}

void testParsesObservedBroadcastNetworkLayout() {
    std::vector<std::uint8_t> payload{
        0x00, 0x00, 0x01, 0xFC,
        0x00, 0x01,
        192, 168, 1, 208,
        255, 255, 255, 0,
        192, 168, 1, 1,
        30,
    };

    const auto packet = buildPacket(LightSpaceNetConfig::PACKET_TYPE_BASIC,
                                    LightSpaceNetConfig::CMD_BROADCAST_RESPONSE,
                                    payload);
    const auto status = LightSpaceNetStatus::parseBroadcastResponse(packet.data(), packet.size());
    ASSERT_TRUE(status.has_value(), "observed broadcast response parses");
    ASSERT_EQ(status->deviceId, static_cast<std::uint32_t>(508), "observed device id");
    ASSERT_EQ(status->firmwareVersion, static_cast<std::uint16_t>(1), "observed firmware");
    ASSERT_EQ(status->hardwareVersion, static_cast<std::uint16_t>(30), "observed hardware byte");
    ASSERT_TRUE(status->ipAddress == "192.168.1.208", "observed ip address");
    ASSERT_TRUE(status->stableId() == "508", "observed stable id");
    ASSERT_TRUE(status->displayLabel() == "LightSpace 508",
                "observed display label uses stable id fallback");
}

void testScanFrequencyClampsToDocumentRange() {
    ASSERT_EQ(LightSpaceNetConfig::scanFrequencyKilohertz(0),
              static_cast<std::uint8_t>(30),
              "zero rate falls back to default scan frequency");
    ASSERT_EQ(LightSpaceNetConfig::scanFrequencyKilohertz(500),
              static_cast<std::uint8_t>(1),
              "low rate clamps to 1 kHz");
    ASSERT_EQ(LightSpaceNetConfig::scanFrequencyKilohertz(150000),
              static_cast<std::uint8_t>(30),
              "high rate clamps to documented 30 kHz limit");
}

void testScanFrequencyPacketUsesKilohertzPayload() {
    const auto packet = buildScanFrequencyPacket(30000);
    const auto parsed = parsePacket(packet.data(), packet.size());
    ASSERT_TRUE(parsed.has_value(), "scan frequency packet parses");
    ASSERT_EQ(parsed->packetType,
              LightSpaceNetConfig::PACKET_TYPE_COMMAND,
              "scan frequency is command class");
    ASSERT_EQ(parsed->commandWord,
              LightSpaceNetConfig::CMD_SET_SCAN_FREQUENCY,
              "scan frequency command word");
    ASSERT_EQ(parsed->payload.size(), static_cast<std::size_t>(1), "scan frequency payload size");
    ASSERT_EQ(parsed->payload[0],
              static_cast<std::uint8_t>(30),
              "30000 pps maps to 30 kHz scan rate");
}

void testPointStreamPacketLengthForCompletePattern() {
    std::vector<core::LaserPoint> points(500);
    const auto packet = buildPointStreamPacket(points);
    const auto parsed = parsePacket(packet.data(), packet.size());

    ASSERT_TRUE(parsed.has_value(), "complete-pattern point stream parses");
    ASSERT_EQ(packet.size(),
              static_cast<std::size_t>(20 + 2 + (500 * 7)),
              "packet length includes header, point count, point data, and crc");
    ASSERT_EQ(parsed->payload.size(),
              static_cast<std::size_t>(2 + (500 * 7)),
              "data length includes point count plus point data");
    ASSERT_EQ(readBe16(parsed->payload.data()),
              static_cast<std::uint16_t>(500),
              "point count is complete pattern size");
}

void testCurrentPatternFirmwareSafePacketLimit() {
    ASSERT_EQ(LightSpaceNetConfig::MEASURED_MAX_SOURCE_FRAME_POINTS,
              static_cast<std::size_t>(728),
              "LightSpace current-pattern measured one-shot point cap");
    ASSERT_EQ(LightSpaceNetConfig::MAX_SOURCE_FRAME_POINTS,
              static_cast<std::size_t>(700),
              "LightSpace current-pattern operational point cap");

    std::vector<core::LaserPoint> maxPoints(
        LightSpaceNetConfig::MEASURED_MAX_SOURCE_FRAME_POINTS);
    const auto maxPacket = buildPointStreamPacket(maxPoints);
    ASSERT_EQ(maxPacket.size(),
              static_cast<std::size_t>(5118),
              "728 points fits below the observed 5120-byte packet cap");

    const auto nextPacketBytes =
        LightSpaceNetConfig::CURRENT_PATTERN_PACKET_OVERHEAD +
        ((LightSpaceNetConfig::MEASURED_MAX_SOURCE_FRAME_POINTS + 1) *
         LightSpaceNetConfig::BYTES_PER_POINT);
    ASSERT_TRUE(nextPacketBytes > LightSpaceNetConfig::MAX_CURRENT_PATTERN_PACKET_BYTES,
                "729 points exceeds the observed 5120-byte packet cap");
}

void testOversizedCurrentPatternFitsWithBlankTravelTail() {
    std::vector<core::LaserPoint> sourcePoints;
    for (std::size_t i = 0; i < 10; ++i) {
        core::LaserPoint point;
        point.x = static_cast<float>(i) / 128.0f;
        point.y = static_cast<float>(i) / 256.0f;
        point.r = 1.0f;
        point.g = 0.5f;
        point.b = 0.25f;
        point.i = 1.0f;
        sourcePoints.push_back(point);
    }

    const auto fitted = fitCurrentPatternToPointLimit(sourcePoints, 6);
    ASSERT_EQ(fitted.size(),
              static_cast<std::size_t>(6),
              "oversized LightSpace pattern is capped to requested size");

    ASSERT_EQ(fitted[0].x, sourcePoints[0].x, "first prefix point is preserved");
    ASSERT_EQ(fitted[2].x, sourcePoints[2].x, "visible prefix is preserved up to the tail");
    ASSERT_EQ(fitted[2].r, 1.0f, "visible prefix colour is preserved");

    ASSERT_EQ(fitted[3].x, sourcePoints[2].x, "blank tail starts from last retained point");
    ASSERT_EQ(fitted[3].r, 0.0f, "blank tail clears red");
    ASSERT_EQ(fitted[3].g, 0.0f, "blank tail clears green");
    ASSERT_EQ(fitted[3].b, 0.0f, "blank tail clears blue");
    ASSERT_EQ(fitted[3].i, 0.0f, "blank tail clears legacy intensity");

    ASSERT_EQ(fitted.back().x, sourcePoints.back().x, "blank tail reaches original end x");
    ASSERT_EQ(fitted.back().y, sourcePoints.back().y, "blank tail reaches original end y");
    ASSERT_EQ(fitted.back().r, 0.0f, "original end point is blanked");

    const auto packet = buildPointStreamPacket(fitted);
    ASSERT_EQ(packet.size(),
              static_cast<std::size_t>(20 + 2 + (6 * 7)),
              "fitted pattern packet uses the capped point count");
}

void testOversizedCurrentPatternUsesFastBlankTravelSpacing() {
    std::vector<core::LaserPoint> sourcePoints(600);
    for (auto& point : sourcePoints) {
        point.x = -1.0f;
        point.y = 0.0f;
        point.r = 1.0f;
        point.i = 1.0f;
    }
    sourcePoints.back().x = 1.0f;

    const auto fitted = fitCurrentPatternToPointLimit(sourcePoints, 100);
    ASSERT_EQ(fitted.size(),
              static_cast<std::size_t>(100),
              "oversized LightSpace pattern is capped to requested size");
    ASSERT_EQ(fitted[48].r, 1.0f,
              "full-width blank travel at 2 percent spacing leaves 49 visible points");
    ASSERT_EQ(fitted[49].r, 0.0f,
              "remaining 51 points are the blank travel tail");
    ASSERT_EQ(fitted.back().x, 1.0f, "blank tail reaches the full-width end point");
    ASSERT_EQ(fitted.back().r, 0.0f, "blank tail endpoint remains blank");
}

void testOversizedCurrentPatternKeepsVisiblePrefixForOutOfRangeEndpoint() {
    std::vector<core::LaserPoint> sourcePoints;
    for (std::size_t i = 0; i < 1000; ++i) {
        core::LaserPoint point;
        point.x = static_cast<float>(i) / 1000.0f;
        point.y = 0.0f;
        point.r = 1.0f;
        point.g = 0.25f;
        point.b = 0.0f;
        point.i = 1.0f;
        sourcePoints.push_back(point);
    }
    sourcePoints.back().x = 100.0f;
    sourcePoints.back().y = -100.0f;

    const auto fitted = fitCurrentPatternToPointLimit(sourcePoints, 500);
    ASSERT_EQ(fitted.size(),
              static_cast<std::size_t>(500),
              "out-of-range oversized pattern is capped to requested size");
    ASSERT_TRUE(fitted[350].r > 0.0f,
                "out-of-range endpoint cannot consume the whole packet as blank travel");
    ASSERT_EQ(fitted.back().x, sourcePoints.back().x, "blank tail still targets original end x");
    ASSERT_EQ(fitted.back().y, sourcePoints.back().y, "blank tail still targets original end y");
    ASSERT_EQ(fitted.back().r, 0.0f, "tail endpoint remains blank");
}

} // namespace

int main() {
    testBroadcastQueryMatchesDocumentExample();
    testParsesBuiltPacket();
    testRejectsBadCrc();
    testPointStreamEncoding();
    testUnsignedPointStreamEncodingCanBeSelected();
    testCoordinateAxisOptions();
    testNarrowCoordinateEncodingsCanBeSelected();
    testCoordinateScaleAndByteOrderCanBeSelected();
    testParsesBroadcastResponse();
    testParsesObservedBroadcastNetworkLayout();
    testScanFrequencyClampsToDocumentRange();
    testScanFrequencyPacketUsesKilohertzPayload();
    testPointStreamPacketLengthForCompletePattern();
    testCurrentPatternFirmwareSafePacketLimit();
    testOversizedCurrentPatternFitsWithBlankTravelTail();
    testOversizedCurrentPatternUsesFastBlankTravelSpacing();
    testOversizedCurrentPatternKeepsVisiblePrefixForOutOfRangeEndpoint();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("LightSpaceNet packet tests passed");
    return 0;
}
