#include "libera/core/BufferEstimator.hpp"
#include "libera/log/Log.hpp"

#include <chrono>

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

static void testEstimateFromSnapshotProjects() {
    const auto now = std::chrono::steady_clock::now();
    const auto snapshotTime = now - std::chrono::milliseconds(10);

    const auto estimate = BufferEstimator::estimateFromSnapshot(
        1000,
        snapshotTime,
        30000,
        now);

    ASSERT_TRUE(estimate.projected, "estimate should be projected");
    ASSERT_EQ(estimate.bufferFullness, 700, "10ms at 30kpps should consume 300 points");
}

static void testEstimateFromSnapshotFallback() {
    const auto now = std::chrono::steady_clock::now();
    const auto estimate = BufferEstimator::estimateFromSnapshot(
        512,
        std::chrono::steady_clock::time_point{},
        30000,
        now);

    ASSERT_TRUE(!estimate.projected, "missing snapshot should not project");
    ASSERT_EQ(estimate.bufferFullness, 512, "fallback keeps snapshot fullness");
}

static void testEstimateFromSnapshotZeroRateFallback() {
    const auto now = std::chrono::steady_clock::now();
    const auto snapshotTime = now - std::chrono::milliseconds(5);
    const auto estimate = BufferEstimator::estimateFromSnapshot(
        420,
        snapshotTime,
        0,
        now);

    ASSERT_TRUE(!estimate.projected, "zero rate should not project");
    ASSERT_EQ(estimate.bufferFullness, 420, "zero-rate fallback keeps snapshot fullness");
}

static void testMinimumBufferPoints() {
    const int highRate = BufferEstimator::minimumBufferPoints(
        30000,
        std::chrono::milliseconds(50),
        256);
    ASSERT_EQ(highRate, 1500, "high rate should be time-based");

    const int lowRate = BufferEstimator::minimumBufferPoints(
        1000,
        std::chrono::milliseconds(50),
        256);
    ASSERT_EQ(lowRate, 256, "low rate should respect floor");
}

static void testTargetBufferPointsUsesLatency() {
    const int target = BufferEstimator::targetBufferPoints(
        30000,
        6000,
        std::chrono::milliseconds(100),
        140,
        280);
    ASSERT_EQ(target, 3000, "100ms at 30kpps should request 3000 points when capacity allows");
}

static void testTargetBufferPointsRespectsSafetyHeadroom() {
    const int target = BufferEstimator::targetBufferPoints(
        30000,
        1799,
        std::chrono::milliseconds(100),
        140,
        280);
    ASSERT_EQ(target, 1519, "high latency should clamp to capacity minus safety headroom");
}

static void testTargetBufferPointsKeepsFloorAtZeroLatency() {
    const int target = BufferEstimator::targetBufferPoints(
        2000,
        1799,
        std::chrono::milliseconds(0),
        140,
        280);
    ASSERT_EQ(target, 140, "zero latency should still respect the minimum buffer floor");
}

static void testClampSleepMillis() {
    ASSERT_EQ(BufferEstimator::clampSleepMillis(0, 1, 50), 1, "lower bound clamp");
    ASSERT_EQ(BufferEstimator::clampSleepMillis(100, 1, 50), 50, "upper bound clamp");
    ASSERT_EQ(BufferEstimator::clampSleepMillis(20, 1, 50), 20, "in-range unchanged");
}

int main() {
    testEstimateFromSnapshotProjects();
    testEstimateFromSnapshotFallback();
    testEstimateFromSnapshotZeroRateFallback();
    testMinimumBufferPoints();
    testTargetBufferPointsUsesLatency();
    testTargetBufferPointsRespectsSafetyHeadroom();
    testTargetBufferPointsKeepsFloorAtZeroLatency();
    testClampSleepMillis();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("BufferEstimator tests passed");
    return 0;
}
