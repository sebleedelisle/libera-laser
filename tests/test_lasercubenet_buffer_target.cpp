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

void testThirtyKppsTargetsTenMilliseconds() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(30000, 1799),
        300,
        "30kpps should target about 10ms of buffered points");
}

void testHigherRateScalesTheTarget() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(60000, 1799),
        600,
        "higher point rates should keep the same time-based buffer");
}

void testLowRateStillKeepsOnePacketBuffered() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(2000, 1799),
        static_cast<int>(LaserCubeNetConfig::MAX_POINTS_PER_PACKET),
        "low rates should still keep one packet buffered");
}

void testSmallBuffersClampToCapacity() {
    ASSERT_EQ(
        LaserCubeNetConfig::targetBufferPoints(30000, 256),
        256,
        "small hardware buffers should clamp the target to capacity");
}

} // namespace

int main() {
    testThirtyKppsTargetsTenMilliseconds();
    testHigherRateScalesTheTarget();
    testLowRateStillKeepsOnePacketBuffered();
    testSmallBuffersClampToCapacity();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("LaserCubeNet buffer target tests passed");
    return 0;
}
