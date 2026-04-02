#include "libera/core/ByteBuffer.hpp"
#include "libera/core/ByteRead.hpp"
#include "libera/log/Log.hpp"

#include <cstdint>

using namespace libera;
using namespace libera::core;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

static void testEmptyAfterConstruction() {
    ByteBuffer buf;
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(0), "new buffer should be empty");
}

static void testAppendChar() {
    ByteBuffer buf;
    buf.appendChar('d');
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(1), "one char = 1 byte");
    ASSERT_EQ(buf.data()[0], static_cast<std::uint8_t>('d'), "char value round-trips");
}

static void testAppendUInt8() {
    ByteBuffer buf;
    buf.appendUInt8(0xAB);
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(1), "one uint8 = 1 byte");
    ASSERT_EQ(buf.data()[0], static_cast<std::uint8_t>(0xAB), "uint8 value round-trips");
}

static void testAppendUInt16LittleEndian() {
    ByteBuffer buf;
    buf.appendUInt16(0x1234);
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(2), "one uint16 = 2 bytes");
    ASSERT_EQ(buf.data()[0], static_cast<std::uint8_t>(0x34), "low byte first");
    ASSERT_EQ(buf.data()[1], static_cast<std::uint8_t>(0x12), "high byte second");
    ASSERT_EQ(bytes::readLe16(buf.data()), static_cast<std::uint16_t>(0x1234),
              "readLe16 round-trips appendUInt16");
}

static void testAppendInt16LittleEndian() {
    ByteBuffer buf;
    buf.appendInt16(-1);
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(2), "one int16 = 2 bytes");
    ASSERT_EQ(buf.data()[0], static_cast<std::uint8_t>(0xFF), "-1 low byte");
    ASSERT_EQ(buf.data()[1], static_cast<std::uint8_t>(0xFF), "-1 high byte");
}

static void testAppendInt16Positive() {
    ByteBuffer buf;
    buf.appendInt16(256);
    ASSERT_EQ(buf.data()[0], static_cast<std::uint8_t>(0x00), "256 low byte");
    ASSERT_EQ(buf.data()[1], static_cast<std::uint8_t>(0x01), "256 high byte");
}

static void testAppendUInt32LittleEndian() {
    ByteBuffer buf;
    buf.appendUInt32(0xDEADBEEF);
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(4), "one uint32 = 4 bytes");
    ASSERT_EQ(buf.data()[0], static_cast<std::uint8_t>(0xEF), "byte 0");
    ASSERT_EQ(buf.data()[1], static_cast<std::uint8_t>(0xBE), "byte 1");
    ASSERT_EQ(buf.data()[2], static_cast<std::uint8_t>(0xAD), "byte 2");
    ASSERT_EQ(buf.data()[3], static_cast<std::uint8_t>(0xDE), "byte 3");
    ASSERT_EQ(bytes::readLe32(buf.data()), static_cast<std::uint32_t>(0xDEADBEEF),
              "readLe32 round-trips appendUInt32");
}

static void testClear() {
    ByteBuffer buf;
    buf.appendUInt32(12345);
    buf.appendUInt16(678);
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(6), "pre-clear size");
    buf.clear();
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(0), "clear resets to empty");
}

static void testSequentialAppends() {
    ByteBuffer buf;
    buf.appendChar('q');
    buf.appendUInt32(30000);

    ASSERT_EQ(buf.size(), static_cast<std::size_t>(5), "char + uint32 = 5 bytes");
    ASSERT_EQ(buf.data()[0], static_cast<std::uint8_t>('q'), "opcode byte");
    ASSERT_EQ(bytes::readLe32(buf.data() + 1), static_cast<std::uint32_t>(30000),
              "payload round-trips after opcode");
}

static void testAppendUInt16Boundary() {
    ByteBuffer buf;
    buf.appendUInt16(0);
    ASSERT_EQ(bytes::readLe16(buf.data()), static_cast<std::uint16_t>(0), "uint16 zero");

    buf.clear();
    buf.appendUInt16(0xFFFF);
    ASSERT_EQ(bytes::readLe16(buf.data()), static_cast<std::uint16_t>(0xFFFF), "uint16 max");
}

static void testAppendUInt32Boundary() {
    ByteBuffer buf;
    buf.appendUInt32(0);
    ASSERT_EQ(bytes::readLe32(buf.data()), static_cast<std::uint32_t>(0), "uint32 zero");

    buf.clear();
    buf.appendUInt32(0xFFFFFFFF);
    ASSERT_EQ(bytes::readLe32(buf.data()), static_cast<std::uint32_t>(0xFFFFFFFF), "uint32 max");
}

int main() {
    testEmptyAfterConstruction();
    testAppendChar();
    testAppendUInt8();
    testAppendUInt16LittleEndian();
    testAppendInt16LittleEndian();
    testAppendInt16Positive();
    testAppendUInt32LittleEndian();
    testClear();
    testSequentialAppends();
    testAppendUInt16Boundary();
    testAppendUInt32Boundary();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("ByteBuffer tests passed");
    return 0;
}
