#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <cstddef>

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

class FrameRequestTestController : public LaserController {
public:
    bool requestFrameNow(std::size_t maximumPointsRequired,
                         std::size_t blankFramePointCount,
                         std::chrono::steady_clock::time_point estimatedFirstPointRenderTime,
                         Frame& outputFrame) {
        FrameFillRequest request{};
        request.maximumPointsRequired = maximumPointsRequired;
        request.blankFramePointCount = blankFramePointCount;
        request.estimatedFirstPointRenderTime = estimatedFirstPointRenderTime;
        return LaserController::requestFrame(request, outputFrame);
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

Frame makeSteppedFrame(float startX, std::size_t count) {
    Frame frame;
    frame.points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        LaserPoint p{};
        p.x = startX + static_cast<float>(i);
        p.r = 1.0f;
        p.g = 1.0f;
        p.b = 1.0f;
        frame.points.push_back(p);
    }
    return frame;
}

void prepareController(FrameRequestTestController& controller,
                       std::uint32_t pointRate = 1000) {
    controller.setPointRate(0);
    controller.setArmed(true);
    controller.setPointRate(pointRate);
    controller.startFrameMode();
}

void testDueQueuedFramePassesThroughUnchanged() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(100));
    prepareController(controller);

    const auto now = std::chrono::steady_clock::now();
    Frame frame = makeSteppedFrame(10.0f, 4);
    frame.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(frame)), "queued frame");

    Frame output;
    ASSERT_TRUE(controller.requestFrameNow(32, 6, now, output), "requestFrame succeeds");
    ASSERT_EQ(output.points.size(), static_cast<std::size_t>(4), "frame size preserved");
    ASSERT_EQ(output.points.front().x, 10.0f, "frame head preserved");
    ASSERT_EQ(output.points.back().x, 13.0f, "frame tail preserved");
    ASSERT_EQ(output.points.front().r, 1.0f, "content colour preserved");
}

void testFutureFrameReturnsIdleBlankFrame() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(100));
    prepareController(controller);

    const auto now = std::chrono::steady_clock::now();
    Frame frame = makeSteppedFrame(10.0f, 4);
    frame.time = now + std::chrono::milliseconds(50);
    ASSERT_TRUE(controller.sendFrame(std::move(frame)), "future frame queued");

    Frame output;
    ASSERT_TRUE(controller.requestFrameNow(32, 6, now, output), "requestFrame succeeds");
    ASSERT_EQ(output.points.size(), static_cast<std::size_t>(6), "blank frame uses requested idle size");
    ASSERT_EQ(output.points.front().x, 0.0f, "blank frame stays centred");
    ASSERT_EQ(output.points.front().r, 0.0f, "blank frame stays dark");
    ASSERT_EQ(output.points.back().i, 0.0f, "blank frame clears legacy intensity");
}

void testHoldLastFrameRepeatsPreviousContent() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(500));
    prepareController(controller);

    const auto now = std::chrono::steady_clock::now();
    Frame frame = makeSteppedFrame(20.0f, 4);
    frame.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(frame)), "frame queued");

    Frame firstOutput;
    ASSERT_TRUE(controller.requestFrameNow(32, 6, now, firstOutput), "first requestFrame succeeds");

    Frame secondOutput;
    ASSERT_TRUE(controller.requestFrameNow(32, 6, now, secondOutput), "held-frame request succeeds");
    ASSERT_EQ(secondOutput.points.size(), static_cast<std::size_t>(4), "held frame keeps original size");
    ASSERT_EQ(secondOutput.points.front().x, 20.0f, "held frame repeats the first point");
    ASSERT_EQ(secondOutput.points.back().x, 23.0f, "held frame repeats the last point");
}

void testTransitionBlankFramePrecedesNextDistantFrame() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(500));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(500));
    prepareController(controller);

    const auto now = std::chrono::steady_clock::now();
    Frame first = makeFrameAt(0.0f, 0.0f, 3);
    first.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(first)), "first frame queued");

    Frame firstOutput;
    ASSERT_TRUE(controller.requestFrameNow(64, 6, now, firstOutput), "first content frame requested");

    Frame second = makeFrameAt(1.0f, 1.0f, 3);
    second.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(second)), "second frame queued");

    Frame transition;
    ASSERT_TRUE(controller.requestFrameNow(64, 6, now, transition), "transition frame requested");
    ASSERT_TRUE(!transition.points.empty(), "transition frame should contain blanks");
    ASSERT_EQ(transition.points.front().i, 0.0f, "transition starts dark");
    ASSERT_EQ(transition.points.back().i, 0.0f, "transition ends dark");
    ASSERT_EQ(transition.points.front().x, 0.0f, "transition dwells at the old endpoint first");
    ASSERT_EQ(transition.points.back().x, 1.0f, "transition finishes at the new endpoint");

    Frame secondOutput;
    ASSERT_TRUE(controller.requestFrameNow(64, 6, now, secondOutput), "next content frame requested");
    ASSERT_EQ(secondOutput.points.size(), static_cast<std::size_t>(3), "next frame size preserved");
    ASSERT_EQ(secondOutput.points.front().x, 1.0f, "next frame starts at new position");
    ASSERT_EQ(secondOutput.points.front().i, 1.0f, "next frame restores content intensity");
}

void testOversizedFrameIsTruncatedAndTailIsDropped() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(500));
    prepareController(controller);

    const auto now = std::chrono::steady_clock::now();
    Frame frame = makeSteppedFrame(30.0f, 8);
    frame.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(frame)), "oversized frame queued");

    Frame firstOutput;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, firstOutput), "first truncated frame requested");
    ASSERT_EQ(firstOutput.points.size(), static_cast<std::size_t>(4), "frame is truncated to backend maximum");
    ASSERT_EQ(firstOutput.points.front().x, 30.0f, "truncated frame keeps leading points");
    ASSERT_EQ(firstOutput.points.back().x, 33.0f, "truncated frame drops tail points");

    Frame secondOutput;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, secondOutput), "held truncated frame requested");
    ASSERT_EQ(secondOutput.points.size(), static_cast<std::size_t>(4), "truncated size is stable on hold");
    ASSERT_EQ(secondOutput.points.front().x, 30.0f, "held frame repeats the truncated head");
    ASSERT_EQ(secondOutput.points.back().x, 33.0f, "held frame never leaks the discarded tail");
}

} // namespace

int main() {
    testDueQueuedFramePassesThroughUnchanged();
    testFutureFrameReturnsIdleBlankFrame();
    testHoldLastFrameRepeatsPreviousContent();
    testTransitionBlankFramePrecedesNextDistantFrame();
    testOversizedFrameIsTruncatedAndTailIsDropped();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Frame request tests passed");
    return 0;
}
