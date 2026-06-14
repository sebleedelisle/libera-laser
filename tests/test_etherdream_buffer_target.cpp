#include "libera/core/BufferEstimator.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/log/Log.hpp"

#include <chrono>

using namespace libera;

static int g_failures = 0;

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

static void testEtherDreamTargetUsesLatencyWhenThereIsRoom() {
    const int target = core::BufferEstimator::targetBufferPoints(
        30000,
        4096,
        std::chrono::milliseconds(50),
        static_cast<int>(etherdream::config::ETHERDREAM_MIN_BUFFER_POINTS),
        static_cast<int>(etherdream::config::ETHERDREAM_SAFETY_HEADROOM_POINTS));

    ASSERT_EQ(target, 1500, "50ms at 30kpps should be honoured when capacity allows");
}

static void testEtherDreamTargetKeepsFreeHeadroom() {
    const int target = core::BufferEstimator::targetBufferPoints(
        30000,
        4096,
        std::chrono::milliseconds(200),
        static_cast<int>(etherdream::config::ETHERDREAM_MIN_BUFFER_POINTS),
        static_cast<int>(etherdream::config::ETHERDREAM_SAFETY_HEADROOM_POINTS));

    ASSERT_EQ(target, 3584, "large latency target should clamp to capacity minus Ether Dream headroom");
}

int main() {
    testEtherDreamTargetUsesLatencyWhenThereIsRoom();
    testEtherDreamTargetKeepsFreeHeadroom();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("EtherDream buffer target tests passed");
    return 0;
}
