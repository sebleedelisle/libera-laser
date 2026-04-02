#include "libera/core/LaserControllerStreaming.hpp"
#include "libera/log/Log.hpp"

#include <cmath>
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

namespace {

// Expose the protected static encoding helpers via a test harness.
class EncodingHarness : public LaserControllerStreaming {
public:
    void run() override {}

    static std::uint16_t u16FromSigned(float v) { return encodeUnsigned16FromSignedUnit(v); }
    static std::uint16_t u16FromUnit(float v)   { return encodeUnsigned16FromUnit(v); }
    static std::uint16_t u12FromSigned(float v) { return encodeUnsigned12FromSignedUnit(v); }
    static std::uint16_t u12FromUnit(float v)   { return encodeUnsigned12FromUnit(v); }
    static std::uint8_t  u8FromUnit(float v)    { return encodeUnsigned8FromUnit(v); }
};

// ── encodeUnsigned16FromSignedUnit ───────────────────────────────────

void testU16Signed_Zero() {
    ASSERT_EQ(EncodingHarness::u16FromSigned(0.0f), static_cast<std::uint16_t>(32768),
              "0.0 maps to midpoint 32768");
}

void testU16Signed_Neg1() {
    ASSERT_EQ(EncodingHarness::u16FromSigned(-1.0f), static_cast<std::uint16_t>(0),
              "-1.0 maps to 0");
}

void testU16Signed_Pos1() {
    ASSERT_EQ(EncodingHarness::u16FromSigned(1.0f), static_cast<std::uint16_t>(65535),
              "1.0 maps to 65535");
}

void testU16Signed_ClampsAbove() {
    ASSERT_EQ(EncodingHarness::u16FromSigned(2.0f), static_cast<std::uint16_t>(65535),
              "values above 1.0 clamp to 65535");
}

void testU16Signed_ClampsBelow() {
    ASSERT_EQ(EncodingHarness::u16FromSigned(-5.0f), static_cast<std::uint16_t>(0),
              "values below -1.0 clamp to 0");
}

// ── encodeUnsigned16FromUnit ─────────────────────────────────────────

void testU16Unit_Zero() {
    ASSERT_EQ(EncodingHarness::u16FromUnit(0.0f), static_cast<std::uint16_t>(0),
              "0.0 maps to 0");
}

void testU16Unit_One() {
    ASSERT_EQ(EncodingHarness::u16FromUnit(1.0f), static_cast<std::uint16_t>(65535),
              "1.0 maps to 65535");
}

void testU16Unit_Half() {
    ASSERT_EQ(EncodingHarness::u16FromUnit(0.5f), static_cast<std::uint16_t>(32768),
              "0.5 maps to 32768");
}

void testU16Unit_ClampsNegative() {
    ASSERT_EQ(EncodingHarness::u16FromUnit(-0.5f), static_cast<std::uint16_t>(0),
              "negative values clamp to 0");
}

void testU16Unit_ClampsAbove() {
    ASSERT_EQ(EncodingHarness::u16FromUnit(1.5f), static_cast<std::uint16_t>(65535),
              "values above 1.0 clamp to 65535");
}

// ── encodeUnsigned12FromSignedUnit ───────────────────────────────────

void testU12Signed_Zero() {
    ASSERT_EQ(EncodingHarness::u12FromSigned(0.0f), static_cast<std::uint16_t>(2048),
              "0.0 maps to midpoint 2048");
}

void testU12Signed_Neg1() {
    ASSERT_EQ(EncodingHarness::u12FromSigned(-1.0f), static_cast<std::uint16_t>(0),
              "-1.0 maps to 0");
}

void testU12Signed_Pos1() {
    ASSERT_EQ(EncodingHarness::u12FromSigned(1.0f), static_cast<std::uint16_t>(4095),
              "1.0 maps to 4095");
}

void testU12Signed_ClampsAbove() {
    ASSERT_EQ(EncodingHarness::u12FromSigned(3.0f), static_cast<std::uint16_t>(4095),
              "values above 1.0 clamp to 4095");
}

// ── encodeUnsigned12FromUnit ─────────────────────────────────────────

void testU12Unit_Zero() {
    ASSERT_EQ(EncodingHarness::u12FromUnit(0.0f), static_cast<std::uint16_t>(0),
              "0.0 maps to 0");
}

void testU12Unit_One() {
    ASSERT_EQ(EncodingHarness::u12FromUnit(1.0f), static_cast<std::uint16_t>(4095),
              "1.0 maps to 4095");
}

void testU12Unit_Half() {
    ASSERT_EQ(EncodingHarness::u12FromUnit(0.5f), static_cast<std::uint16_t>(2048),
              "0.5 maps to 2048");
}

void testU12Unit_ClampsNegative() {
    ASSERT_EQ(EncodingHarness::u12FromUnit(-1.0f), static_cast<std::uint16_t>(0),
              "negative values clamp to 0");
}

// ── encodeUnsigned8FromUnit ──────────────────────────────────────────

void testU8Unit_Zero() {
    ASSERT_EQ(EncodingHarness::u8FromUnit(0.0f), static_cast<std::uint8_t>(0),
              "0.0 maps to 0");
}

void testU8Unit_One() {
    ASSERT_EQ(EncodingHarness::u8FromUnit(1.0f), static_cast<std::uint8_t>(255),
              "1.0 maps to 255");
}

void testU8Unit_Half() {
    ASSERT_EQ(EncodingHarness::u8FromUnit(0.5f), static_cast<std::uint8_t>(128),
              "0.5 maps to 128");
}

void testU8Unit_ClampsNegative() {
    ASSERT_EQ(EncodingHarness::u8FromUnit(-1.0f), static_cast<std::uint8_t>(0),
              "negative values clamp to 0");
}

void testU8Unit_ClampsAbove() {
    ASSERT_EQ(EncodingHarness::u8FromUnit(2.0f), static_cast<std::uint8_t>(255),
              "values above 1.0 clamp to 255");
}

} // namespace

int main() {
    testU16Signed_Zero();
    testU16Signed_Neg1();
    testU16Signed_Pos1();
    testU16Signed_ClampsAbove();
    testU16Signed_ClampsBelow();

    testU16Unit_Zero();
    testU16Unit_One();
    testU16Unit_Half();
    testU16Unit_ClampsNegative();
    testU16Unit_ClampsAbove();

    testU12Signed_Zero();
    testU12Signed_Neg1();
    testU12Signed_Pos1();
    testU12Signed_ClampsAbove();

    testU12Unit_Zero();
    testU12Unit_One();
    testU12Unit_Half();
    testU12Unit_ClampsNegative();

    testU8Unit_Zero();
    testU8Unit_One();
    testU8Unit_Half();
    testU8Unit_ClampsNegative();
    testU8Unit_ClampsAbove();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Encoding helper tests passed");
    return 0;
}
