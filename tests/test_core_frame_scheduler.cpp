#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <cstddef>
#include <vector>

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

class FrameSchedulerTestController : public LaserController {
public:
    const std::vector<LaserPoint>& lastBatch() const {
        return pointsToSend;
    }

protected:
    void run() override {}
};

Frame makeFrame(float start, std::size_t count) {
    Frame frame;
    frame.points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        LaserPoint p{};
        p.x = start + static_cast<float>(i);
        frame.points.push_back(p);
    }
    return frame;
}

void testDefaultTargetLatencyMatchesOfxLaser() {
    ASSERT_EQ(LaserController::targetLatency().count(),
              100LL,
              "libera default target latency should match ofxLaser");
}

void testEmptyQueuePadsOnlyToMinimum() {
    FrameSchedulerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setArmed(true);
    controller.startFrameMode();

    PointFillRequest request{};
    request.minimumPointsRequired = 4;
    request.maximumPointsRequired = 16;
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();

    ASSERT_TRUE(controller.requestPoints(request), "empty-queue requestPoints succeeds");
    const auto batch = controller.lastBatch();
    ASSERT_EQ(batch.size(), static_cast<std::size_t>(4), "idle blank batch should honor the minimum contract");
    ASSERT_EQ(batch.front().x, 0.0f, "idle blank batch should keep x at origin");
    ASSERT_EQ(batch.front().r, 0.0f, "idle blank batch should remain dark");
    ASSERT_EQ(batch.back().y, 0.0f, "idle blank batch should keep y at origin");
    ASSERT_EQ(batch.back().b, 0.0f, "idle blank batch should remain dark at the tail");
}

void testDoesNotDropFrameMidPlayback() {
    FrameSchedulerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setArmed(true);
    controller.startFrameMode();

    ASSERT_TRUE(controller.sendFrame(makeFrame(10.0f, 10)), "first frame queued");

    PointFillRequest request{};
    request.minimumPointsRequired = 1;
    request.maximumPointsRequired = 5;
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();

    ASSERT_TRUE(controller.requestPoints(request), "first requestPoints succeeds");
    const auto firstBatch = controller.lastBatch();
    ASSERT_EQ(firstBatch.size(), static_cast<std::size_t>(5), "first batch size");
    ASSERT_EQ(firstBatch[0].x, 10.0f, "first batch starts at frame1 point0");
    ASSERT_EQ(firstBatch[4].x, 14.0f, "first batch ends at frame1 point4");

    ASSERT_TRUE(controller.sendFrame(makeFrame(100.0f, 10)), "second frame queued");

    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(controller.requestPoints(request), "second requestPoints succeeds");
    const auto secondBatch = controller.lastBatch();
    ASSERT_EQ(secondBatch.size(), static_cast<std::size_t>(5), "second batch size");
    ASSERT_EQ(secondBatch[0].x, 15.0f, "second batch continues frame1");
    ASSERT_EQ(secondBatch[4].x, 19.0f, "second batch finishes frame1");
}

void testHighLatencyAllowsMultipleFramesQueued() {
    FrameSchedulerTestController controller;
    controller.setPointRate(30000);
    LaserController::setTargetLatency(std::chrono::milliseconds(100));
    controller.setArmed(true);
    controller.startFrameMode();

    std::size_t acceptedFrames = 0;
    for (; acceptedFrames < 20; ++acceptedFrames) {
        if (!controller.sendFrame(makeFrame(static_cast<float>(acceptedFrames * 1000), 500))) {
            break;
        }
    }

    ASSERT_TRUE(acceptedFrames >= 6, "100ms latency should admit a multi-frame lead buffer");
    ASSERT_TRUE(acceptedFrames < 20, "queue should still apply backpressure");
    ASSERT_EQ(controller.queuedFrameCount(), acceptedFrames, "queued frame count matches pending frames");
}

void testReadinessReopensAsQueuedPointsDrain() {
    FrameSchedulerTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setArmed(true);
    controller.startFrameMode();

    PointFillRequest request{};
    request.minimumPointsRequired = 5;
    request.maximumPointsRequired = 5;
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();

    ASSERT_TRUE(controller.sendFrame(makeFrame(10.0f, 10)), "first frame queued");
    ASSERT_TRUE(controller.requestPoints(request), "first partial drain succeeds");
    ASSERT_TRUE(controller.sendFrame(makeFrame(100.0f, 10)), "second frame queued");
    ASSERT_TRUE(!controller.isReadyForNewFrame(), "remaining current+future points still exceed budget");

    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();
    ASSERT_TRUE(controller.requestPoints(request), "current frame finishes cleanly");
    ASSERT_TRUE(controller.isReadyForNewFrame(), "budget reopens once only one frame of points remains");
}

void testPromotesFrameWhenItBecomesDueMidBatch() {
    FrameSchedulerTestController controller;
    controller.setPointRate(1000);
    LaserController::setTargetLatency(std::chrono::milliseconds(20));
    controller.setArmed(true);
    controller.startFrameMode();

    // Use spatially compact frames (all at the origin) to avoid transition
    // blanking interfering with the scheduling test. Distinguish frames by
    // the r channel: frame1 has r=0.1, frame2 has r=0.9.
    auto makeCompactFrame = [](float rValue, std::size_t count) {
        Frame f;
        f.points.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            LaserPoint p{};
            p.r = rValue;
            f.points.push_back(p);
        }
        return f;
    };

    const auto startTime = std::chrono::steady_clock::now();
    Frame frame1 = makeCompactFrame(0.1f, 10);
    frame1.time = startTime;
    ASSERT_TRUE(controller.sendFrame(std::move(frame1)), "first timed frame queued");

    // Start frame1 playing (consume 1 point) so it won't be treated as stale
    // when frame2 is queued alongside it.
    PointFillRequest drain{};
    drain.minimumPointsRequired = 1;
    drain.maximumPointsRequired = 1;
    drain.estimatedFirstPointRenderTime = startTime;
    ASSERT_TRUE(controller.requestPoints(drain), "drain start point");

    Frame frame2 = makeCompactFrame(0.9f, 10);
    frame2.time = startTime + std::chrono::milliseconds(1);
    ASSERT_TRUE(controller.sendFrame(std::move(frame2)), "second timed frame queued");

    PointFillRequest request{};
    request.minimumPointsRequired = 14;
    request.maximumPointsRequired = 14;
    request.estimatedFirstPointRenderTime = startTime;

    ASSERT_TRUE(controller.requestPoints(request), "mid-batch transition request succeeds");
    const auto batch = controller.lastBatch();
    ASSERT_EQ(batch.size(), static_cast<std::size_t>(14), "batch size");
    // First 9 points come from frame1 (r=0.1), then frame2 is promoted.
    // Note: startup blank zeroes r on the first requestPoints call, but
    // the second call is past the startup window.
    ASSERT_EQ(batch[0].r, 0.1f, "batch continues first frame");
    ASSERT_EQ(batch[8].r, 0.1f, "frame1 tail");
    ASSERT_EQ(batch[9].r, 0.9f, "second frame starts as soon as it becomes due");
}

void testFutureFrameOnlyRequestsMinimumBlankLead() {
    FrameSchedulerTestController controller;
    controller.setPointRate(1000);
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setArmed(true);
    controller.startFrameMode();

    Frame futureFrame = makeFrame(10.0f, 10);
    futureFrame.time = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    ASSERT_TRUE(controller.sendFrame(std::move(futureFrame)), "future frame queued");

    PointFillRequest request{};
    request.minimumPointsRequired = 2;
    request.maximumPointsRequired = 10;
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();

    ASSERT_TRUE(controller.requestPoints(request), "future-frame requestPoints succeeds");
    const auto batch = controller.lastBatch();
    ASSERT_EQ(batch.size(), static_cast<std::size_t>(2), "future frame should only be padded to the minimum blank lead");
    ASSERT_EQ(batch[0].x, 0.0f, "future-frame lead should stay blank");
    ASSERT_EQ(batch[1].r, 0.0f, "future-frame lead should stay dark");
}

} // namespace

int main() {
    testDefaultTargetLatencyMatchesOfxLaser();
    testEmptyQueuePadsOnlyToMinimum();
    testDoesNotDropFrameMidPlayback();
    testHighLatencyAllowsMultipleFramesQueued();
    testReadinessReopensAsQueuedPointsDrain();
    testPromotesFrameWhenItBecomesDueMidBatch();
    testFutureFrameOnlyRequestsMinimumBlankLead();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Frame scheduler tests passed");
    return 0;
}
