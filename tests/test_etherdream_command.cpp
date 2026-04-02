#include "libera/etherdream/EtherDreamCommand.hpp"
#include "libera/core/ByteRead.hpp"
#include "libera/log/Log.hpp"

#include <cmath>
#include <cstdint>

using namespace libera;
using namespace libera::etherdream;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

// ── Single-byte commands ─────────────────────────────────────────────

static void testSingleByteCommand() {
    EtherDreamCommand cmd;
    cmd.setSingleByteCommand('c');
    ASSERT_TRUE(cmd.isReady(), "single-byte command should be ready");
    ASSERT_EQ(cmd.commandOpcode(), 'c', "opcode should match");
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(1), "single-byte command = 1 byte");
    ASSERT_EQ(cmd.data()[0], static_cast<std::uint8_t>('c'), "byte value");
}

static void testPingCommand() {
    EtherDreamCommand cmd;
    cmd.setSingleByteCommand('?');
    ASSERT_EQ(cmd.commandOpcode(), '?', "ping opcode");
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(1), "ping = 1 byte");
}

// ── Begin command ────────────────────────────────────────────────────

static void testBeginCommand() {
    EtherDreamCommand cmd;
    cmd.setBeginCommand(30000);
    ASSERT_TRUE(cmd.isReady(), "begin command should be ready");
    ASSERT_EQ(cmd.commandOpcode(), 'b', "begin opcode");
    // 'b' (1 byte) + reserved flags (2 bytes) + point rate (4 bytes) = 7 bytes
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(7), "begin command = 7 bytes");
    ASSERT_EQ(cmd.data()[0], static_cast<std::uint8_t>('b'), "opcode byte");
    // Reserved flags at offset 1..2 should be zero.
    ASSERT_EQ(core::bytes::readLe16(cmd.data() + 1), static_cast<std::uint16_t>(0),
              "reserved flags zero");
    ASSERT_EQ(core::bytes::readLe32(cmd.data() + 3), static_cast<std::uint32_t>(30000),
              "point rate");
}

// ── Point rate command ───────────────────────────────────────────────

static void testPointRateCommand() {
    EtherDreamCommand cmd;
    cmd.setPointRateCommand(45000);
    ASSERT_TRUE(cmd.isReady(), "point rate command should be ready");
    ASSERT_EQ(cmd.commandOpcode(), 'q', "rate change opcode");
    // 'q' (1 byte) + point rate (4 bytes) = 5 bytes
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(5), "rate command = 5 bytes");
    ASSERT_EQ(core::bytes::readLe32(cmd.data() + 1), static_cast<std::uint32_t>(45000),
              "point rate value");
}

// ── Data command + point encoding ────────────────────────────────────

static void testDataCommandHeader() {
    EtherDreamCommand cmd;
    cmd.setDataCommand(10);
    ASSERT_TRUE(cmd.isReady(), "data command should be ready after header");
    ASSERT_EQ(cmd.commandOpcode(), 'd', "data opcode");
    // 'd' (1 byte) + point count (2 bytes) = 3 bytes header
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(3), "data header = 3 bytes");
    ASSERT_EQ(core::bytes::readLe16(cmd.data() + 1), static_cast<std::uint16_t>(10),
              "point count in header");
}

static void testPointEncodingOriginBlack() {
    EtherDreamCommand cmd;
    cmd.setDataCommand(1);

    core::LaserPoint point{};
    // x=0, y=0, r=0, g=0, b=0, i=1 (default), u1=0, u2=0
    cmd.addPoint(point, false);

    // Header 3 bytes + 1 point * 18 bytes = 21 bytes
    // Each point: control(2) + x(2) + y(2) + r(2) + g(2) + b(2) + i(2) + u1(2) + u2(2) = 18 bytes
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(21), "header + 1 point");

    const auto* p = cmd.data() + 3; // skip header
    auto control = core::bytes::readLe16(p);
    ASSERT_EQ(control, static_cast<std::uint16_t>(0), "control without rate change flag");

    // x=0, y=0 -> both should encode to 0 as signed int16
    auto xVal = static_cast<std::int16_t>(core::bytes::readLe16(p + 2));
    auto yVal = static_cast<std::int16_t>(core::bytes::readLe16(p + 4));
    ASSERT_EQ(xVal, static_cast<std::int16_t>(0), "x=0 encodes to 0");
    ASSERT_EQ(yVal, static_cast<std::int16_t>(0), "y=0 encodes to 0");

    // r, g, b = 0 -> 0
    ASSERT_EQ(core::bytes::readLe16(p + 6), static_cast<std::uint16_t>(0), "r=0");
    ASSERT_EQ(core::bytes::readLe16(p + 8), static_cast<std::uint16_t>(0), "g=0");
    ASSERT_EQ(core::bytes::readLe16(p + 10), static_cast<std::uint16_t>(0), "b=0");

    // i defaults to 1.0 -> 65535
    ASSERT_EQ(core::bytes::readLe16(p + 12), static_cast<std::uint16_t>(65535), "i=1.0 -> 65535");
}

static void testPointEncodingFullBright() {
    EtherDreamCommand cmd;
    cmd.setDataCommand(1);

    core::LaserPoint point{};
    point.x = 1.0f;
    point.y = -1.0f;
    point.r = 1.0f;
    point.g = 1.0f;
    point.b = 1.0f;
    point.i = 1.0f;
    cmd.addPoint(point, false);

    const auto* p = cmd.data() + 3;
    auto xVal = static_cast<std::int16_t>(core::bytes::readLe16(p + 2));
    auto yVal = static_cast<std::int16_t>(core::bytes::readLe16(p + 4));
    ASSERT_EQ(xVal, static_cast<std::int16_t>(32767), "x=1.0 -> 32767");
    ASSERT_EQ(yVal, static_cast<std::int16_t>(-32767), "y=-1.0 -> -32767");
    ASSERT_EQ(core::bytes::readLe16(p + 6), static_cast<std::uint16_t>(65535), "r=1.0 -> 65535");
    ASSERT_EQ(core::bytes::readLe16(p + 8), static_cast<std::uint16_t>(65535), "g=1.0 -> 65535");
    ASSERT_EQ(core::bytes::readLe16(p + 10), static_cast<std::uint16_t>(65535), "b=1.0 -> 65535");
}

static void testPointEncodingClamps() {
    EtherDreamCommand cmd;
    cmd.setDataCommand(1);

    core::LaserPoint point{};
    point.x = 5.0f;   // way over 1.0
    point.y = -5.0f;  // way under -1.0
    point.r = 2.0f;   // over 1.0
    point.g = -1.0f;  // under 0.0
    cmd.addPoint(point, false);

    const auto* p = cmd.data() + 3;
    auto xVal = static_cast<std::int16_t>(core::bytes::readLe16(p + 2));
    auto yVal = static_cast<std::int16_t>(core::bytes::readLe16(p + 4));
    ASSERT_EQ(xVal, static_cast<std::int16_t>(32767), "x clamped to max");
    ASSERT_EQ(yVal, static_cast<std::int16_t>(-32767), "y clamped to min");
    ASSERT_EQ(core::bytes::readLe16(p + 6), static_cast<std::uint16_t>(65535), "r clamped to max");
    ASSERT_EQ(core::bytes::readLe16(p + 8), static_cast<std::uint16_t>(0), "g clamped to 0");
}

static void testRateChangeFlag() {
    EtherDreamCommand cmd;
    cmd.setDataCommand(1);

    core::LaserPoint point{};
    cmd.addPoint(point, true);

    const auto* p = cmd.data() + 3;
    auto control = core::bytes::readLe16(p);
    ASSERT_EQ(control, static_cast<std::uint16_t>(0x8000), "rate change bit should be set");
}

static void testMultiplePoints() {
    EtherDreamCommand cmd;
    cmd.setDataCommand(3);

    for (int i = 0; i < 3; ++i) {
        core::LaserPoint point{};
        point.x = static_cast<float>(i) * 0.5f;
        cmd.addPoint(point, false);
    }

    // 3 bytes header + 3 * 18 bytes points = 57 bytes
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(57), "header + 3 points");
}

// ── Reset ────────────────────────────────────────────────────────────

static void testReset() {
    EtherDreamCommand cmd;
    cmd.setBeginCommand(30000);
    ASSERT_TRUE(cmd.isReady(), "ready before reset");
    cmd.reset();
    ASSERT_TRUE(!cmd.isReady(), "not ready after reset");
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(0), "size zero after reset");
}

static void testResetBetweenCommands() {
    EtherDreamCommand cmd;
    cmd.setSingleByteCommand('c');
    ASSERT_EQ(cmd.commandOpcode(), 'c', "first command opcode");

    cmd.setBeginCommand(10000);
    ASSERT_EQ(cmd.commandOpcode(), 'b', "second command replaces first");
    ASSERT_EQ(cmd.size(), static_cast<std::size_t>(7), "new command size");
}

int main() {
    testSingleByteCommand();
    testPingCommand();
    testBeginCommand();
    testPointRateCommand();
    testDataCommandHeader();
    testPointEncodingOriginBlack();
    testPointEncodingFullBright();
    testPointEncodingClamps();
    testRateChangeFlag();
    testMultiplePoints();
    testReset();
    testResetBetweenCommands();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("EtherDream command tests passed");
    return 0;
}
