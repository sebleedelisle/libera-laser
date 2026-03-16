#include "libera/core/LaserControllerStreaming.hpp"
#include "libera/log/Log.hpp"

#include <cstdint>
#include <string>
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

} // namespace

int main() {
    testDefaultStatusIsRed();
    testGreenOrangeTransitions();
    testConnectionFailureIsRedAndCounted();
    testClearErrorsResetsCounts();
    testArmRisingEdgeResetsStartupBlank();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Controller status tests passed");
    return 0;
}
