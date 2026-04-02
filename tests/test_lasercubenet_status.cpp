#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/log/Log.hpp"

#include <array>
#include <cstdint>
#include <cstring>

using namespace libera;
using namespace libera::lasercubenet;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

// Build a minimal 64-byte status packet with the given fields.
std::array<std::uint8_t, 64> makeStatusPacket(
    std::uint8_t firmwareMajor, std::uint8_t firmwareMinor,
    std::uint8_t flags,
    std::uint32_t pointRate, std::uint32_t pointRateMax,
    std::uint16_t bufferFree, std::uint16_t bufferMax,
    std::uint8_t batteryPercent, std::uint8_t temperatureC,
    std::uint8_t connectionType,
    const std::uint8_t serial[6],
    std::uint8_t ip0, std::uint8_t ip1, std::uint8_t ip2, std::uint8_t ip3,
    std::uint8_t modelNumber, const char* modelName)
{
    std::array<std::uint8_t, 64> data{};
    data[2] = 0; // payload version 0
    data[3] = firmwareMajor;
    data[4] = firmwareMinor;
    data[5] = flags;

    // point rate at offset 10 (LE32)
    data[10] = static_cast<std::uint8_t>(pointRate & 0xFF);
    data[11] = static_cast<std::uint8_t>((pointRate >> 8) & 0xFF);
    data[12] = static_cast<std::uint8_t>((pointRate >> 16) & 0xFF);
    data[13] = static_cast<std::uint8_t>((pointRate >> 24) & 0xFF);

    // point rate max at offset 14 (LE32)
    data[14] = static_cast<std::uint8_t>(pointRateMax & 0xFF);
    data[15] = static_cast<std::uint8_t>((pointRateMax >> 8) & 0xFF);
    data[16] = static_cast<std::uint8_t>((pointRateMax >> 16) & 0xFF);
    data[17] = static_cast<std::uint8_t>((pointRateMax >> 24) & 0xFF);

    // buffer free at offset 19 (LE16)
    data[19] = static_cast<std::uint8_t>(bufferFree & 0xFF);
    data[20] = static_cast<std::uint8_t>((bufferFree >> 8) & 0xFF);

    // buffer max at offset 21 (LE16)
    data[21] = static_cast<std::uint8_t>(bufferMax & 0xFF);
    data[22] = static_cast<std::uint8_t>((bufferMax >> 8) & 0xFF);

    data[23] = batteryPercent;
    data[24] = temperatureC;
    data[25] = connectionType;

    // serial at offset 26..31
    std::memcpy(&data[26], serial, 6);

    // IP at offset 32..35
    data[32] = ip0;
    data[33] = ip1;
    data[34] = ip2;
    data[35] = ip3;

    data[37] = modelNumber;

    // model name at offset 38
    if (modelName) {
        const std::size_t len = std::min(std::strlen(modelName), static_cast<std::size_t>(64 - 38));
        std::memcpy(&data[38], modelName, len);
    }

    return data;
}

void testRejectsShort() {
    ASSERT_TRUE(!LaserCubeNetStatus::parse(nullptr, 0).has_value(),
                "null data should fail");

    std::array<std::uint8_t, 32> small{};
    ASSERT_TRUE(!LaserCubeNetStatus::parse(small.data(), small.size()).has_value(),
                "under 64 bytes should fail");
}

void testRejectsUnknownPayloadVersion() {
    std::array<std::uint8_t, 64> data{};
    data[2] = 1; // unknown version
    ASSERT_TRUE(!LaserCubeNetStatus::parse(data.data(), data.size()).has_value(),
                "payload version 1 should be rejected");
}

void testParsesBasicFields() {
    const std::uint8_t serial[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    auto packet = makeStatusPacket(
        1, 5,       // firmware 1.5
        0x01,       // output enabled only
        30000, 40000,
        500, 2000,
        85, 42, 1,
        serial,
        192, 168, 1, 100,
        3, "LaserCube");

    auto result = LaserCubeNetStatus::parse(packet.data(), packet.size());
    ASSERT_TRUE(result.has_value(), "parse should succeed");

    const auto& s = *result;
    ASSERT_EQ(s.firmwareMajor, static_cast<std::uint8_t>(1), "firmware major");
    ASSERT_EQ(s.firmwareMinor, static_cast<std::uint8_t>(5), "firmware minor");
    ASSERT_TRUE(s.firmwareVersion == "1.5", "firmware version string");
    ASSERT_EQ(s.pointRate, static_cast<std::uint32_t>(30000), "point rate");
    ASSERT_EQ(s.pointRateMax, static_cast<std::uint32_t>(40000), "point rate max");
    ASSERT_EQ(s.bufferFree, static_cast<std::uint16_t>(500), "buffer free");
    ASSERT_EQ(s.bufferMax, static_cast<std::uint16_t>(2000), "buffer max");
    ASSERT_EQ(s.batteryPercent, static_cast<std::uint8_t>(85), "battery");
    ASSERT_EQ(s.temperatureC, static_cast<std::uint8_t>(42), "temperature");
    ASSERT_EQ(s.connectionType, static_cast<std::uint8_t>(1), "connection type");
    ASSERT_TRUE(s.serialNumber == "AABBCCDDEEFF", "serial number hex");
    ASSERT_TRUE(s.ipAddress == "192.168.1.100", "IP address");
    ASSERT_EQ(s.modelNumber, static_cast<std::uint8_t>(3), "model number");
    ASSERT_TRUE(s.modelName == "LaserCube", "model name");
}

void testOutputEnabledFlag() {
    const std::uint8_t serial[6] = {};
    auto packet = makeStatusPacket(1, 13, 0x01, 0, 0, 0, 0, 0, 0, 0, serial, 0, 0, 0, 0, 0, "");
    auto result = LaserCubeNetStatus::parse(packet.data(), packet.size());
    ASSERT_TRUE(result.has_value(), "parse succeeds");
    ASSERT_TRUE(result->outputEnabled, "output should be enabled");
    ASSERT_TRUE(!result->interlockEnabled, "interlock should be disabled");
}

void testNewFlagLayout() {
    // firmware >= 0.13 uses new flag layout
    const std::uint8_t serial[6] = {};
    // flags: output=1, interlock=1, tempWarning=1, overTemp=1, packetErrors=3
    const std::uint8_t flags = 0x01 | 0x02 | 0x04 | 0x08 | (3 << 4);
    auto packet = makeStatusPacket(0, 13, flags, 0, 0, 0, 0, 0, 0, 0, serial, 0, 0, 0, 0, 0, "");
    auto result = LaserCubeNetStatus::parse(packet.data(), packet.size());
    ASSERT_TRUE(result.has_value(), "parse succeeds");
    ASSERT_TRUE(result->outputEnabled, "output enabled (new layout)");
    ASSERT_TRUE(result->interlockEnabled, "interlock enabled (new layout)");
    ASSERT_TRUE(result->temperatureWarning, "temp warning (new layout)");
    ASSERT_TRUE(result->overTemperature, "over temp (new layout)");
    ASSERT_EQ(result->packetErrors, static_cast<std::uint8_t>(3), "packet errors (new layout)");
}

void testOldFlagLayout() {
    // firmware < 0.13 uses old flag layout
    const std::uint8_t serial[6] = {};
    // Old layout: interlock=0x08, tempWarning=0x10, overTemp=0x20
    const std::uint8_t flags = 0x01 | 0x08 | 0x10 | 0x20;
    auto packet = makeStatusPacket(0, 12, flags, 0, 0, 0, 0, 0, 0, 0, serial, 0, 0, 0, 0, 0, "");
    auto result = LaserCubeNetStatus::parse(packet.data(), packet.size());
    ASSERT_TRUE(result.has_value(), "parse succeeds");
    ASSERT_TRUE(result->outputEnabled, "output enabled (old layout)");
    ASSERT_TRUE(result->interlockEnabled, "interlock enabled (old layout)");
    ASSERT_TRUE(result->temperatureWarning, "temp warning (old layout)");
    ASSERT_TRUE(result->overTemperature, "over temp (old layout)");
}

void testNewFlagLayoutFirmwareMajor1() {
    // firmware 1.0 should use new layout even though minor < 13
    const std::uint8_t serial[6] = {};
    const std::uint8_t flags = 0x02; // interlock bit in new layout
    auto packet = makeStatusPacket(1, 0, flags, 0, 0, 0, 0, 0, 0, 0, serial, 0, 0, 0, 0, 0, "");
    auto result = LaserCubeNetStatus::parse(packet.data(), packet.size());
    ASSERT_TRUE(result.has_value(), "parse succeeds");
    ASSERT_TRUE(!result->outputEnabled, "output disabled");
    ASSERT_TRUE(result->interlockEnabled, "firmware 1.0 uses new layout, interlock bit at 0x02");
}

} // namespace

int main() {
    testRejectsShort();
    testRejectsUnknownPayloadVersion();
    testParsesBasicFields();
    testOutputEnabledFlag();
    testNewFlagLayout();
    testOldFlagLayout();
    testNewFlagLayoutFirmwareMajor1();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("LaserCubeNet status tests passed");
    return 0;
}
