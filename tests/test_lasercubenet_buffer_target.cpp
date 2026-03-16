#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/log/Log.hpp"

using namespace libera;
using namespace libera::lasercubenet;

static int g_failures = 0;

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

void testLowLatencyKeepsOnePacketMinimum() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(2000, 1799, std::chrono::milliseconds(0)),
        static_cast<int>(LaserCubeNetConfig::MAX_POINTS_PER_PACKET),
        "zero latency should still keep one packet buffered");
}

void testLatencyDrivesThirtyKppsTarget() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(30000, 1799, std::chrono::milliseconds(10)),
        300,
        "30kpps should target latency-derived buffered points");
}

void testHigherRateScalesTheTarget() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(60000, 1799, std::chrono::milliseconds(10)),
        600,
        "higher point rates should scale the latency-derived target");
}

void testHighLatencyClampsToSafetyHeadroomOnSmallBuffer() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(30000, 1799, std::chrono::milliseconds(100)),
        1519,
        "high latency should fill close to full while preserving safety headroom");
}

void testHighLatencyUsesLargeBufferCapacityWhenAvailable() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(30000, 6000, std::chrono::milliseconds(100)),
        3000,
        "large hardware buffers should be able to absorb the configured latency");
}

void testVerySmallBuffersLeaveHeadroom() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(30000, 256, std::chrono::milliseconds(100)),
        140,
        "small hardware buffers should still leave safety headroom");
}

} // namespace

int main() {
    testLowLatencyKeepsOnePacketMinimum();
    testLatencyDrivesThirtyKppsTarget();
    testHigherRateScalesTheTarget();
    testHighLatencyClampsToSafetyHeadroomOnSmallBuffer();
    testHighLatencyUsesLargeBufferCapacityWhenAvailable();
    testVerySmallBuffersLeaveHeadroom();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("LaserCubeNet buffer target tests passed");
    return 0;
}
