#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

#include <cmath>
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

class TransitionTestController : public LaserController {
public:
    const std::vector<LaserPoint>& lastBatch() const {
        return pointsToSend;
    }

    bool requestPoints(const PointFillRequest& request) {
        return LaserController::requestPoints(request);
    }

protected:
    void run() override {}
};

Frame makeFrameAt(float x, float y, std::size_t count) {
    Frame frame;
    frame.points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        LaserPoint p{};
        p.x = x;
        p.y = y;
        p.r = 1.0f;
        p.g = 1.0f;
        p.b = 1.0f;
        frame.points.push_back(p);
    }
    return frame;
}

// ── Transition blanking between distant frames ───────────────────────

void testTransitionBlankingInsertedForDistantFrames() {
    TransitionTestController controller;
    controller.setPointRate(1000);
    LaserController::setTargetLatency(std::chrono::milliseconds(500));
    controller.setArmed(true);
    controller.startFrameMode();

    const auto now = std::chrono::steady_clock::now();

    // Send frame1 first and start it playing so it won't be skipped as stale.
    Frame f1 = makeFrameAt(0.0f, 0.0f, 5);
    f1.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(f1)), "first frame queued");

    // Drain 1 point from frame1 (+ consume startup blank).
    PointFillRequest drain{};
    drain.minimumPointsRequired = 1;
    drain.maximumPointsRequired = 1;
    drain.estimatedFirstPointRenderTime = now;
    controller.requestPoints(drain);

    // Now send frame2 — frame1 already has nextPoint>0 so won't be skipped.
    Frame f2 = makeFrameAt(1.0f, 1.0f, 5);
    f2.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(f2)), "second frame queued");

    // Request enough to force past frame1 boundary into frame2.
    PointFillRequest request{};
    request.minimumPointsRequired = 50;
    request.maximumPointsRequired = 200;
    request.estimatedFirstPointRenderTime = now;

    ASSERT_TRUE(controller.requestPoints(request), "requestPoints succeeds");
    const auto& batch = controller.lastBatch();

    // Should have: 4 frame1 + N transition blanks + frame2 content + looping.
    ASSERT_TRUE(batch.size() >= 50, "batch should meet minimum");

    // First 4 points should be frame1 at x=0.
    ASSERT_EQ(batch[0].x, 0.0f, "frame 1 point at origin x");

    // Find where transition starts (first point with i=0 after frame1).
    std::size_t transitionStart = 0;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        if (batch[i].i == 0.0f) {
            transitionStart = i;
            break;
        }
    }
    ASSERT_TRUE(transitionStart >= 4, "transition should start after frame 1 content");
    ASSERT_TRUE(transitionStart <= 4, "transition should start right after frame 1 ends");

    // Find where frame2 content starts (first point at x=1 with i=1).
    std::size_t frame2Start = 0;
    for (std::size_t i = transitionStart; i < batch.size(); ++i) {
        if (batch[i].x == 1.0f && batch[i].i == 1.0f) {
            frame2Start = i;
            break;
        }
    }
    ASSERT_TRUE(frame2Start > transitionStart, "frame2 should come after transition");

    // All transition points should have i=0.
    for (std::size_t i = transitionStart; i < frame2Start; ++i) {
        ASSERT_EQ(batch[i].i, 0.0f, "transition point should have i=0");
    }
}

void testNoTransitionForCloseFrames() {
    TransitionTestController controller;
    controller.setPointRate(1000);
    LaserController::setTargetLatency(std::chrono::milliseconds(500));
    controller.setArmed(true);
    controller.startFrameMode();

    const auto now = std::chrono::steady_clock::now();

    // Send frame1 and start it playing.
    Frame f1 = makeFrameAt(0.0f, 0.0f, 5);
    f1.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(f1)), "first frame queued");

    PointFillRequest drain{};
    drain.minimumPointsRequired = 1;
    drain.maximumPointsRequired = 1;
    drain.estimatedFirstPointRenderTime = now;
    controller.requestPoints(drain);

    // Frame2 very close — distance < 0.2 threshold.
    Frame f2 = makeFrameAt(0.05f, 0.05f, 5);
    f2.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(f2)), "second frame queued");

    PointFillRequest request{};
    request.minimumPointsRequired = 50;
    request.maximumPointsRequired = 200;
    request.estimatedFirstPointRenderTime = now;

    ASSERT_TRUE(controller.requestPoints(request), "requestPoints succeeds");
    const auto& batch = controller.lastBatch();

    // Close frames: no transition blanking injected. Check first 9 points
    // are all content (i=1.0).
    bool foundTransitionBlank = false;
    for (std::size_t i = 0; i < std::min(batch.size(), static_cast<std::size_t>(9)); ++i) {
        if (batch[i].i == 0.0f) {
            foundTransitionBlank = true;
            break;
        }
    }
    ASSERT_TRUE(!foundTransitionBlank,
                "close frames should not insert transition blanks");
}

void testTransitionPointsDwellAtBothEnds() {
    TransitionTestController controller;
    controller.setPointRate(1000);
    LaserController::setTargetLatency(std::chrono::milliseconds(500));
    controller.setArmed(true);
    controller.startFrameMode();

    const auto now = std::chrono::steady_clock::now();

    Frame f1 = makeFrameAt(0.0f, 0.0f, 3);
    f1.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(f1)), "first frame queued");

    // Start playing frame1.
    PointFillRequest drain{};
    drain.minimumPointsRequired = 1;
    drain.maximumPointsRequired = 1;
    drain.estimatedFirstPointRenderTime = now;
    controller.requestPoints(drain);

    Frame f2 = makeFrameAt(1.0f, 0.0f, 3);
    f2.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(f2)), "second frame queued");

    PointFillRequest request{};
    request.minimumPointsRequired = 50;
    request.maximumPointsRequired = 200;
    request.estimatedFirstPointRenderTime = now;

    ASSERT_TRUE(controller.requestPoints(request), "requestPoints succeeds");
    const auto& batch = controller.lastBatch();

    // Verify transition contains dwells at source (x=0) and destination (x=1).
    bool foundSourceDwell = false;
    bool foundDestDwell = false;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        if (batch[i].i == 0.0f && batch[i].x == 0.0f) foundSourceDwell = true;
        if (batch[i].i == 0.0f && batch[i].x == 1.0f) foundDestDwell = true;
    }
    ASSERT_TRUE(foundSourceDwell, "transition should dwell at source position");
    ASSERT_TRUE(foundDestDwell, "transition should dwell at destination position");
}

void testHoldLastFrameOnExhaustion() {
    TransitionTestController controller;
    controller.setPointRate(1000);
    LaserController::setTargetLatency(std::chrono::milliseconds(500));
    controller.setArmed(true);
    controller.startFrameMode();

    const auto now = std::chrono::steady_clock::now();
    Frame f1 = makeFrameAt(0.5f, 0.5f, 4);
    f1.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(f1)), "frame queued");

    // Drain startup blank + 2 points.
    PointFillRequest request{};
    request.minimumPointsRequired = 3;
    request.maximumPointsRequired = 3;
    request.estimatedFirstPointRenderTime = now;
    ASSERT_TRUE(controller.requestPoints(request), "first drain succeeds");

    // Second request: needs more points than frame has remaining — should loop.
    request.minimumPointsRequired = 4;
    request.maximumPointsRequired = 4;
    request.estimatedFirstPointRenderTime = now;
    ASSERT_TRUE(controller.requestPoints(request), "second drain succeeds");
    const auto& batch = controller.lastBatch();
    ASSERT_EQ(batch.size(), static_cast<std::size_t>(4), "should produce requested points via looping");
}

void testEmptyFrameRejected() {
    TransitionTestController controller;
    controller.setPointRate(1000);
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    controller.setArmed(true);
    controller.startFrameMode();

    Frame empty;
    ASSERT_TRUE(!controller.sendFrame(std::move(empty)), "empty frame should be rejected");
}

} // namespace

int main() {
    testTransitionBlankingInsertedForDistantFrames();
    testNoTransitionForCloseFrames();
    testTransitionPointsDwellAtBothEnds();
    testHoldLastFrameOnExhaustion();
    testEmptyFrameRejected();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Frame transition tests passed");
    return 0;
}
