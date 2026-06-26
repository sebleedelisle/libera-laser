#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/core/LaserControllerStreaming.hpp"
#include "libera/log/Log.hpp"

#include <cstdint>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

class ControllerStatusHarness : public LaserControllerStreaming {
public:
    void run() override {}

    void setConnected(bool connected) {
        setConnectionState(connected);
    }

    void addIntermittent(const char* type) {
        recordIntermittentError(type);
    }

    void addConnectionFailure(const char* type) {
        recordConnectionError(type);
    }

    const std::vector<LaserPoint>& lastBatch() const {
        return pointsToSend;
    }

    bool requestPoints(const PointFillRequest& request) {
        return LaserControllerStreaming::requestPoints(request);
    }
};

std::unordered_map<std::string, std::uint64_t>
toMap(const std::vector<ControllerErrorInfo>& errors) {
    std::unordered_map<std::string, std::uint64_t> out;
    for (const auto& error : errors) {
        out[error.code] = error.count;
    }
    return out;
}

void testDefaultStatusIsRed() {
    ControllerStatusHarness controller;
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Error,
                "default status should be error until connected");
}

void testGreenOrangeTransitions() {
    ControllerStatusHarness controller;
    controller.setConnected(true);
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Good,
                "connected + no intermittent errors should be good");

    controller.addIntermittent("network.packet_loss");
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Issues,
                "connected + intermittent errors should be issues");

    controller.clearErrors();
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Good,
                "clearErrors should restore good when connected");
}

void testConnectionFailureIsRedAndCounted() {
    ControllerStatusHarness controller;
    controller.setConnected(true);
    controller.addIntermittent("network.buffer_underflow");
    controller.addIntermittent("network.buffer_underflow");
    controller.addConnectionFailure("network.connection_lost");

    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Error,
                "connection failure should force error");

    const auto counts = toMap(controller.getErrors());
    ASSERT_EQ(counts.at("network.buffer_underflow"), static_cast<std::uint64_t>(2),
              "underflow count");
    ASSERT_EQ(counts.at("network.connection_lost"), static_cast<std::uint64_t>(1),
              "connection lost count");
}

void testNetworkUnderflowHasDisplayLabel() {
    const auto label = error_types::labelFor(error_types::network::bufferUnderflow);
    ASSERT_TRUE(label == std::string_view("Network underrun"),
                "network underrun label");
}

void testEtherDreamPlaybackIdleHasDisplayLabel() {
    const auto label = error_types::labelFor(error_types::etherdream::playbackIdle);
    ASSERT_TRUE(label == std::string_view("Ether Dream underrun / playback idle"),
                "Ether Dream playback idle label");
}

void testEtherDreamStreamStarvationHasDisplayLabel() {
    const auto label = error_types::labelFor(error_types::etherdream::streamStarvation);
    ASSERT_TRUE(label == std::string_view("Computer performance underrun"),
                "Ether Dream stream starvation label");
}

void testEtherDreamStopConditionHasDisplayLabel() {
    const auto label = error_types::labelFor(error_types::etherdream::stopCondition);
    ASSERT_TRUE(label == std::string_view("Ether Dream stop condition"),
                "Ether Dream stop condition label");
}

void testClearErrorsResetsCounts() {
    ControllerStatusHarness controller;
    controller.setConnected(true);
    controller.addIntermittent("usb.timeout");
    controller.addIntermittent("usb.timeout");
    controller.addIntermittent("usb.status_error");

    ASSERT_TRUE(!controller.getErrors().empty(), "errors should be present before clear");
    controller.clearErrors();
    ASSERT_TRUE(controller.getErrors().empty(), "errors should be empty after clear");
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Good,
                "status should be good after clear when connected");
}

void testIntermittentStatusExpiresWithoutClearingCounts() {
    using namespace std::chrono_literals;

    ControllerStatusHarness controller;
    controller.setConnected(true);
    controller.setRecentEventHoldTime(20ms);
    controller.addIntermittent("network.packet_loss");

    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Issues,
                "recent warning should make connected controller issues");
    auto recent = controller.getRecentEvent();
    ASSERT_TRUE(recent.has_value(), "recent warning event should be queryable");
    ASSERT_TRUE(recent->severity == ControllerEventSeverity::Warning,
                "recent warning severity");

    std::this_thread::sleep_for(30ms);
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Good,
                "warning status should expire without clearErrors");

    const auto counts = toMap(controller.getErrors());
    ASSERT_EQ(counts.at("network.packet_loss"), static_cast<std::uint64_t>(1),
              "warning count remains after status expires");
}

void testRecoveredConnectionErrorExpiresWithoutClearingCounts() {
    using namespace std::chrono_literals;

    ControllerStatusHarness controller;
    controller.setConnected(true);
    controller.setRecentEventHoldTime(20ms);
    controller.addConnectionFailure("network.connection_lost");

    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Error,
                "active connection failure should be error");

    controller.setConnected(true);
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Error,
                "recovered connection error should stay recent error briefly");

    auto recent = controller.getRecentEvent();
    ASSERT_TRUE(recent.has_value(), "recent error event should be queryable");
    ASSERT_TRUE(recent->severity == ControllerEventSeverity::Error,
                "recent error severity");

    std::this_thread::sleep_for(30ms);
    ASSERT_TRUE(controller.getStatus() == ControllerStatus::Good,
                "recovered error status should expire without clearErrors");

    const auto counts = toMap(controller.getErrors());
    ASSERT_EQ(counts.at("network.connection_lost"), static_cast<std::uint64_t>(1),
              "connection error count remains after status expires");
}

void testArmRisingEdgeResetsStartupBlank() {
    ControllerStatusHarness controller;
    controller.setPointRate(1000);
    controller.setRequestPointsCallback(
        [](const PointFillRequest& req, std::vector<LaserPoint>& out) {
            for (std::size_t i = 0; i < req.maximumPointsRequired; ++i) {
                LaserPoint point{};
                point.r = 1.0f;
                point.g = 1.0f;
                point.b = 1.0f;
                out.push_back(point);
            }
        });

    PointFillRequest request{};
    request.minimumPointsRequired = 2;
    request.maximumPointsRequired = 2;

    controller.setArmed(true);
    ASSERT_TRUE(controller.requestPoints(request), "requestPoints should succeed after arming");
    ASSERT_EQ(controller.lastBatch()[0].r, 0.0f, "first point should be blanked after arm");
    ASSERT_EQ(controller.lastBatch()[1].r, 1.0f, "blanking should only consume the startup window");

    ASSERT_TRUE(controller.requestPoints(request), "second requestPoints should succeed");
    ASSERT_EQ(controller.lastBatch()[0].r, 1.0f, "startup blank should not repeat while still armed");

    controller.setArmed(true);
    ASSERT_TRUE(controller.requestPoints(request), "repeated setArmed(true) should not reblank");
    ASSERT_EQ(controller.lastBatch()[0].r, 1.0f, "setArmed(true) while already armed should not reset blanking");

    controller.setArmed(false);
    controller.setArmed(true);
    ASSERT_TRUE(controller.requestPoints(request), "re-arm requestPoints should succeed");
    ASSERT_EQ(controller.lastBatch()[0].r, 0.0f, "re-arming should reset startup blank");
}

void testPointCallbackSanitizesNonFinitePoints() {
    ControllerStatusHarness controller;
    controller.setPointRate(0);
    controller.setArmed(true);
    controller.setRequestPointsCallback(
        [](const PointFillRequest&, std::vector<LaserPoint>& out) {
            LaserPoint badPosition{};
            badPosition.x = std::numeric_limits<float>::quiet_NaN();
            badPosition.y = 0.25f;
            badPosition.r = 1.0f;
            badPosition.g = 1.0f;
            badPosition.b = 1.0f;
            badPosition.i = 1.0f;
            out.push_back(badPosition);

            LaserPoint clamped{};
            clamped.x = 2.0f;
            clamped.y = -2.0f;
            clamped.r = std::numeric_limits<float>::quiet_NaN();
            clamped.g = 2.0f;
            clamped.b = 0.5f;
            clamped.i = std::numeric_limits<float>::infinity();
            out.push_back(clamped);
        });

    PointFillRequest request{};
    request.minimumPointsRequired = 2;
    request.maximumPointsRequired = 2;

    ASSERT_TRUE(controller.requestPoints(request), "requestPoints should run callback");
    const auto& points = controller.lastBatch();
    ASSERT_EQ(points.size(), static_cast<std::size_t>(2), "sanitized batch keeps point count");

    ASSERT_EQ(points[0].x, 0.0f, "non-finite position moves to centre x");
    ASSERT_EQ(points[0].y, 0.0f, "non-finite position moves to centre y");
    ASSERT_EQ(points[0].r, 0.0f, "non-finite position blanks red");
    ASSERT_EQ(points[0].g, 0.0f, "non-finite position blanks green");
    ASSERT_EQ(points[0].b, 0.0f, "non-finite position blanks blue");
    ASSERT_EQ(points[0].i, 0.0f, "non-finite position blanks intensity");

    ASSERT_EQ(points[1].x, 2.0f, "finite x passes through at core ingress");
    ASSERT_EQ(points[1].y, -2.0f, "finite y passes through at core ingress");
    ASSERT_EQ(points[1].r, 0.0f, "non-finite red channel blanks");
    ASSERT_EQ(points[1].g, 2.0f, "finite green passes through at core ingress");
    ASSERT_EQ(points[1].b, 0.5f, "finite blue passes through");
    ASSERT_EQ(points[1].i, 0.0f, "non-finite intensity blanks");
}

} // namespace

int main() {
    testDefaultStatusIsRed();
    testGreenOrangeTransitions();
    testConnectionFailureIsRedAndCounted();
    testNetworkUnderflowHasDisplayLabel();
    testEtherDreamPlaybackIdleHasDisplayLabel();
    testEtherDreamStreamStarvationHasDisplayLabel();
    testEtherDreamStopConditionHasDisplayLabel();
    testClearErrorsResetsCounts();
    testIntermittentStatusExpiresWithoutClearingCounts();
    testRecoveredConnectionErrorExpiresWithoutClearingCounts();
    testArmRisingEdgeResetsStartupBlank();
    testPointCallbackSanitizesNonFinitePoints();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Controller status tests passed");
    return 0;
}
