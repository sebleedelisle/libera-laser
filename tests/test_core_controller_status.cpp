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

} // namespace

int main() {
    testDefaultStatusIsRed();
    testGreenOrangeTransitions();
    testConnectionFailureIsRedAndCounted();
    testClearErrorsResetsCounts();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("Controller status tests passed");
    return 0;
}
