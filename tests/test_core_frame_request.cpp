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
                         Frame& outputFrame,
                         std::size_t preferredPointCount = 0) {
        FrameFillRequest request{};
        request.maximumPointsRequired = maximumPointsRequired;
        request.preferredPointCount = preferredPointCount;
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
    controller.useFrameQueue();
}

void preparePointCallbackController(FrameRequestTestController& controller,
                                    std::uint32_t pointRate = 1000) {
    controller.setPointRate(0);
    controller.setArmed(true);
    controller.setPointRate(pointRate);
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

void testOversizedFrameIsDeliveredAcrossMultipleTransportFrames() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(500));
    prepareController(controller);

    const auto now = std::chrono::steady_clock::now();
    Frame frame = makeSteppedFrame(30.0f, 8);
    frame.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(frame)), "oversized frame queued");

    Frame firstOutput;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, firstOutput), "first chunk requested");
    ASSERT_EQ(firstOutput.points.size(), static_cast<std::size_t>(4), "oversized frame is split to backend maximum");
    ASSERT_EQ(firstOutput.points.front().x, 30.0f, "first chunk keeps the leading points");
    ASSERT_EQ(firstOutput.points.back().x, 33.0f, "first chunk covers the first transport-sized slice");

    Frame secondOutput;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, secondOutput), "second chunk requested");
    ASSERT_EQ(secondOutput.points.size(), static_cast<std::size_t>(4), "second chunk stays transport-sized");
    ASSERT_EQ(secondOutput.points.front().x, 34.0f, "second chunk resumes from the logical frame cursor");
    ASSERT_EQ(secondOutput.points.back().x, 37.0f, "second chunk reaches the original frame end");

    Frame thirdOutput;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, thirdOutput), "held oversized frame loops cleanly");
    ASSERT_EQ(thirdOutput.points.size(), static_cast<std::size_t>(4), "looped chunk stays transport-sized");
    ASSERT_EQ(thirdOutput.points.front().x, 30.0f, "loop restart returns to the true frame head");
    ASSERT_EQ(thirdOutput.points.back().x, 33.0f, "loop restart preserves logical frame order");
}

void testOversizedFrameFinishesItsLoopBeforeSwitchingFrames() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(500));
    prepareController(controller);

    const auto now = std::chrono::steady_clock::now();
    Frame first = makeSteppedFrame(70.0f, 8);
    first.time = now;
    ASSERT_TRUE(controller.sendFrame(std::move(first)), "first oversized frame queued");

    Frame firstChunk;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, firstChunk), "first chunk requested");
    ASSERT_EQ(firstChunk.points.front().x, 70.0f, "first chunk starts at the logical frame head");
    ASSERT_EQ(firstChunk.points.back().x, 73.0f, "first chunk consumes the first slice");

    Frame second = makeFrameAt(77.0f, 0.0f, 4);
    second.time = now;
    for (auto& point : second.points) {
        point.r = 0.5f;
    }
    ASSERT_TRUE(controller.sendFrame(std::move(second)), "replacement frame queued");

    Frame secondChunk;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, secondChunk), "current logical loop finishes before switch");
    ASSERT_EQ(secondChunk.points.front().x, 74.0f, "second chunk resumes the oversized logical frame");
    ASSERT_EQ(secondChunk.points.back().x, 77.0f, "second chunk reaches the original frame endpoint");
    ASSERT_EQ(secondChunk.points.front().r, 1.0f, "second chunk still belongs to the original frame");

    Frame replacementOutput;
    ASSERT_TRUE(controller.requestFrameNow(4, 6, now, replacementOutput), "replacement frame plays after full loop");
    ASSERT_EQ(replacementOutput.points.size(), static_cast<std::size_t>(4), "replacement frame size is preserved");
    ASSERT_EQ(replacementOutput.points.front().x, 77.0f, "replacement frame starts once the logical loop completes");
    ASSERT_EQ(replacementOutput.points.front().r, 0.5f, "replacement frame content is emitted after the switch");
}

void testPointCallbackCanFillOneTransportFrame() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(500));
    preparePointCallbackController(controller);

    std::size_t nextPointIndex = 0;
    controller.setPointCallback(
        [&nextPointIndex](const PointFillRequest& request, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < request.maximumPointsRequired; ++i) {
                LaserPoint point{};
                point.x = 40.0f + static_cast<float>(nextPointIndex++);
                point.r = 1.0f;
                point.g = 1.0f;
                point.b = 1.0f;
                point.i = 1.0f;
                out.push_back(point);
            }
        });

    Frame output;
    ASSERT_TRUE(controller.requestFrameNow(32,
                                           6,
                                           std::chrono::steady_clock::now(),
                                           output,
                                           5),
                "requestFrame succeeds in point-callback mode");
    ASSERT_EQ(output.points.size(), static_cast<std::size_t>(5), "preferred frame size is used for callback adaptation");
    ASSERT_EQ(output.points.front().x, 40.0f, "callback frame keeps first callback point");
    ASSERT_EQ(output.points.back().x, 44.0f, "callback frame keeps exact callback batch size");
    ASSERT_EQ(output.points.front().i, 1.0f, "callback frame keeps content intensity");
}

void testPointCallbackFrameRequestHonoursBackendMaximum() {
    FrameRequestTestController controller;
    LaserController::setTargetLatency(std::chrono::milliseconds(0));
    LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(500));
    preparePointCallbackController(controller);

    std::size_t nextPointIndex = 0;
    controller.setPointCallback(
        [&nextPointIndex](const PointFillRequest& request, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < request.maximumPointsRequired; ++i) {
                LaserPoint point{};
                point.x = 60.0f + static_cast<float>(nextPointIndex++);
                point.r = 1.0f;
                point.g = 1.0f;
                point.b = 1.0f;
                point.i = 1.0f;
                out.push_back(point);
            }
        });

    Frame output;
    ASSERT_TRUE(controller.requestFrameNow(4,
                                           6,
                                           std::chrono::steady_clock::now(),
                                           output,
                                           8),
                "requestFrame succeeds with oversized preferred point count");
    ASSERT_EQ(output.points.size(), static_cast<std::size_t>(4), "callback adaptation clamps to backend maximum");
    ASSERT_EQ(output.points.front().x, 60.0f, "clamped callback frame keeps the leading points");
    ASSERT_EQ(output.points.back().x, 63.0f, "clamped callback frame drops the excess callback tail");
}

} // namespace

int main() {
    testDueQueuedFramePassesThroughUnchanged();
    testFutureFrameReturnsIdleBlankFrame();
    testHoldLastFrameRepeatsPreviousContent();
    testTransitionBlankFramePrecedesNextDistantFrame();
    testOversizedFrameIsDeliveredAcrossMultipleTransportFrames();
    testOversizedFrameFinishesItsLoopBeforeSwitchingFrames();
    testPointCallbackCanFillOneTransportFrame();
    testPointCallbackFrameRequestHonoursBackendMaximum();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Frame request tests passed");
    return 0;
}
