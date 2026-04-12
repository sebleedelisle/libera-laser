#include "libera/core/LaserController.hpp"
#include "libera/log/Log.hpp"

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

class ContentSourceTestController : public LaserController {
public:
    bool requestPointsNow(const PointFillRequest& request) {
        return LaserController::requestPoints(request);
    }

    const std::vector<LaserPoint>& lastBatch() const {
        return pointsToSend;
    }

protected:
    void run() override {}
};

Frame makeFrameAt(float x, std::size_t count) {
    Frame frame;
    frame.points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        LaserPoint point{};
        point.x = x;
        point.r = 1.0f;
        point.g = 1.0f;
        point.b = 1.0f;
        frame.points.push_back(point);
    }
    return frame;
}

void prepareController(ContentSourceTestController& controller) {
    controller.setPointRate(0);
    controller.setArmed(true);
    controller.setPointRate(1000);
}

void testInstallingCallbackClearsQueuedFrames() {
    ContentSourceTestController controller;
    prepareController(controller);
    controller.startFrameMode();

    ASSERT_TRUE(controller.sendFrame(makeFrameAt(0.25f, 4)), "frame queued");
    ASSERT_EQ(controller.queuedFrameCount(), static_cast<std::size_t>(1), "frame queue contains queued frame");

    controller.setRequestPointsCallback(
        [](const PointFillRequest& request, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < request.maximumPointsRequired; ++i) {
                LaserPoint point{};
                point.x = 0.75f;
                point.r = 1.0f;
                point.g = 1.0f;
                point.b = 1.0f;
                out.push_back(point);
            }
        });

    ASSERT_EQ(controller.queuedFrameCount(), static_cast<std::size_t>(0), "installing callback clears queued frames");

    PointFillRequest request{};
    request.minimumPointsRequired = 1;
    request.maximumPointsRequired = 1;
    ASSERT_TRUE(controller.requestPointsNow(request), "requestPoints succeeds in user callback mode");
    ASSERT_EQ(controller.lastBatch()[0].x, 0.75f, "user callback output is used after source switch");
}

void testStartingFrameModeClearsUserCallback() {
    ContentSourceTestController controller;
    prepareController(controller);

    controller.setRequestPointsCallback(
        [](const PointFillRequest& request, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < request.maximumPointsRequired; ++i) {
                LaserPoint point{};
                point.x = 0.9f;
                point.r = 1.0f;
                point.g = 1.0f;
                point.b = 1.0f;
                out.push_back(point);
            }
        });

    controller.startFrameMode();

    PointFillRequest request{};
    request.minimumPointsRequired = 1;
    request.maximumPointsRequired = 1;
    ASSERT_TRUE(controller.requestPointsNow(request), "requestPoints succeeds in frame mode");
    ASSERT_EQ(controller.lastBatch()[0].x, 0.0f, "empty frame queue blanks instead of using stale callback");
    ASSERT_EQ(controller.lastBatch()[0].r, 0.0f, "frame-mode blank stays dark");
}

} // namespace

int main() {
    testInstallingCallbackClearsQueuedFrames();
    testStartingFrameModeClearsUserCallback();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Content source switch tests passed");
    return 0;
}
