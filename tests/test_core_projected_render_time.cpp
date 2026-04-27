// Verifies LaserController::projectedNextWriteRenderTime() — the shared
// projection that frame-ingester transports use to drive the FrameScheduler's
// due-time gate. The Helios USB path used to compute this as
// `now + writeLead`, which is too small by roughly one DAC frame period and
// caused the FrameScheduler to loop the current frame whenever Liberation's
// per-frame submission gap exceeded the DAC's per-frame play time. The
// projection is shared so all frame-first transports (Helios USB, IDN,
// AVB-frame, frame-first plugins, …) get the same fix.

#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <cstdint>

using namespace libera;
using namespace libera::core;
using namespace std::chrono_literals;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace {

// Test fixture: noteFrameTransportSubmission* and projectedNextWriteRenderTime
// are protected on LaserController; expose them for the unit test without
// requiring a full transport backend.
class ProjectionFixture : public LaserController {
public:
    using LaserController::projectedNextWriteRenderTime;
    using LaserController::noteFrameTransportSubmission;
    using LaserController::noteFrameTransportSubmissionBounded;
    using LaserController::clearFrameTransportSubmissionEstimate;

protected:
    void run() override {}
};

// Convert a duration to whole microseconds as a signed 64-bit integer for
// stable comparisons in the assertions below.
constexpr std::int64_t toMicros(std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}

void testNoSubmissionFallsBackToWriteLead() {
    ProjectionFixture controller;
    const auto now = std::chrono::steady_clock::now();
    const auto writeLead = std::chrono::microseconds(2500);

    const auto projected = controller.projectedNextWriteRenderTime(now, writeLead);

    ASSERT_EQ(toMicros(projected - now),
              2500LL,
              "with no submission recorded, projection collapses to now + writeLead");
}

void testProjectionAdvancesByDrainTimeOfRecordedSnapshot() {
    ProjectionFixture controller;
    const auto submissionTime = std::chrono::steady_clock::now();
    const std::uint32_t pps = 30000;

    // Simulate Helios's call sequence: the controller writes a 1200-point
    // frame whose first sample is projected to render at submissionTime.
    controller.noteFrameTransportSubmissionBounded(
        /*pointCount=*/1200,
        /*estimatedFirstPointRenderTime=*/submissionTime,
        /*pointRateValue=*/pps,
        /*maxCarryOverPoints=*/1200);

    // Slightly later (the worker thread wakes for the next iteration).
    const auto laterNow = submissionTime + std::chrono::milliseconds(5);
    const auto writeLead = std::chrono::microseconds(3000);
    const auto projected = controller.projectedNextWriteRenderTime(laterNow, writeLead);

    // 1200 pts at 30k pps = 40 ms drain. snapshotTime was submissionTime, so
    // drain completes at submissionTime + 40ms. The projection should land
    // there (well above the now + writeLead floor of laterNow + 3ms).
    const auto expected = submissionTime + std::chrono::milliseconds(40);
    ASSERT_EQ(toMicros(projected - expected),
              0LL,
              "projection equals snapshotTime + snapshotPoints/pointRate");
}

void testFloorAppliesWhenProjectionIsAlreadyInThePast() {
    ProjectionFixture controller;
    const auto submissionTime = std::chrono::steady_clock::now();
    const std::uint32_t pps = 30000;
    controller.noteFrameTransportSubmissionBounded(
        /*pointCount=*/300, // 10ms at 30k pps
        submissionTime,
        pps,
        /*maxCarryOverPoints=*/300);

    // Worker wakes 200ms later — the recorded snapshot has long since
    // drained. Projection must still respect the now + writeLead floor so we
    // don't claim a write can happen in the past.
    const auto laterNow = submissionTime + std::chrono::milliseconds(200);
    const auto writeLead = std::chrono::microseconds(4000);
    const auto projected = controller.projectedNextWriteRenderTime(laterNow, writeLead);

    ASSERT_EQ(toMicros(projected - laterNow),
              4000LL,
              "stale snapshot collapses projection to now + writeLead floor");
}

void testRepeatedHeliosCadenceDoesNotDrift() {
    // Simulate a Helios-style write cadence: each call's snapshotTime equals
    // the previous call's drain-completion. Without the carry-over fix, the
    // snapshot kept a stale "remaining at now" carry that double-counted the
    // currently-playing frame, and the projection drifted forward by ~one
    // frame period on every iteration. With the fix, each successive
    // projection should advance by exactly one frame's playback time.
    ProjectionFixture controller;
    const auto baseTime = std::chrono::steady_clock::now();
    const std::uint32_t pps = 30000;
    const std::size_t framePoints = 1200; // 40 ms at 30 kpps.
    const auto framePeriod = std::chrono::microseconds(
        (static_cast<std::int64_t>(framePoints) * 1'000'000) / pps);

    // First write: no previous snapshot. Use baseTime as the projected start.
    controller.noteFrameTransportSubmissionBounded(
        framePoints, baseTime, pps, framePoints);

    auto projection = controller.projectedNextWriteRenderTime(
        baseTime, std::chrono::microseconds(3000));
    auto expected = baseTime + framePeriod;
    ASSERT_EQ(toMicros(projection - expected), 0LL,
              "after one write, projection lands at one frame period ahead");

    // Simulate four more writes, each scheduled at the previous projection.
    // Each iteration's projection should advance by exactly framePeriod.
    for (int i = 0; i < 4; ++i) {
        controller.noteFrameTransportSubmissionBounded(
            framePoints, projection, pps, framePoints);

        projection = controller.projectedNextWriteRenderTime(
            baseTime + framePeriod * (i + 1),
            std::chrono::microseconds(3000));
        expected += framePeriod;

        ASSERT_EQ(toMicros(projection - expected), 0LL,
                  "projection advances by exactly one frame period per iteration");
    }
}

void testFutureSnapshotTimePushesProjectionForward() {
    // When a backend records a submission whose first-point-render-time is
    // already in the future (snapshotTime > now), the projection must use
    // that future snapshotTime as its base — otherwise the projected start
    // of the next-after-this write would land at "now + drain", which is
    // earlier than reality and would re-introduce the FrameScheduler's
    // not-yet-due loop bug.
    ProjectionFixture controller;
    const auto submissionNow = std::chrono::steady_clock::now();
    const std::uint32_t pps = 30000;

    // Mimic the first Helios write where the just-submitted 600pt frame is
    // projected to start playing 5ms from now (deeper firmware buffer).
    const auto futureRenderTime = submissionNow + std::chrono::milliseconds(5);
    controller.noteFrameTransportSubmissionBounded(
        /*pointCount=*/600,
        /*estimatedFirstPointRenderTime=*/futureRenderTime,
        pps,
        /*maxCarryOverPoints=*/600);

    const auto writeLead = std::chrono::microseconds(2000);
    const auto projected = controller.projectedNextWriteRenderTime(submissionNow, writeLead);

    // 600 / 30k = 20ms. snapshotTime is futureRenderTime. So projection of
    // the *next* write's render time = futureRenderTime + 20ms ≈ now + 25ms,
    // not just now + writeLead.
    const auto expected = futureRenderTime + std::chrono::milliseconds(20);
    ASSERT_EQ(toMicros(projected - expected),
              0LL,
              "projection uses recorded future snapshotTime, not just now+writeLead");
    ASSERT_TRUE(projected > submissionNow + writeLead,
                "future snapshotTime must dominate the writeLead floor");
}

void testClearResetsBackToWriteLeadFloor() {
    ProjectionFixture controller;
    const auto submissionTime = std::chrono::steady_clock::now();
    controller.noteFrameTransportSubmissionBounded(
        1200,
        submissionTime + std::chrono::milliseconds(100),
        30000,
        1200);

    controller.clearFrameTransportSubmissionEstimate();

    const auto now = std::chrono::steady_clock::now();
    const auto writeLead = std::chrono::microseconds(1500);
    const auto projected = controller.projectedNextWriteRenderTime(now, writeLead);

    ASSERT_EQ(toMicros(projected - now),
              1500LL,
              "clearFrameTransportSubmissionEstimate() falls back to writeLead floor");
}

void testZeroPointRateSnapshotFallsBackToFloor() {
    ProjectionFixture controller;
    const auto submissionTime = std::chrono::steady_clock::now();

    // Pass pointRate=0 — current behaviour resolves it via getPointRate().
    // Force the snapshot into a 0-rate state by ensuring the controller's
    // own rate is 0 too. (LaserControllerStreaming's default rate is 30000;
    // this exercises the projection's defensive guard against div-by-zero.)
    controller.setPointRate(0);
    controller.noteFrameTransportSubmissionBounded(
        1200,
        submissionTime,
        /*pointRateValue=*/0,
        1200);

    const auto laterNow = submissionTime + std::chrono::milliseconds(5);
    const auto writeLead = std::chrono::microseconds(2000);
    const auto projected = controller.projectedNextWriteRenderTime(laterNow, writeLead);

    ASSERT_EQ(toMicros(projected - laterNow),
              2000LL,
              "zero-rate snapshot must not divide by zero; falls back to writeLead floor");
}

} // namespace

int main() {
    testNoSubmissionFallsBackToWriteLead();
    testProjectionAdvancesByDrainTimeOfRecordedSnapshot();
    testFloorAppliesWhenProjectionIsAlreadyInThePast();
    testRepeatedHeliosCadenceDoesNotDrift();
    testFutureSnapshotTimePushesProjectionForward();
    testClearResetsBackToWriteLeadFloor();
    testZeroPointRateSnapshotFallsBackToFloor();

    if (g_failures == 0) {
        logInfo("test_core_projected_render_time: all tests passed");
        return 0;
    }
    logError("test_core_projected_render_time: failures =", g_failures);
    return 1;
}
