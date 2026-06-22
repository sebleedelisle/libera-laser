#include "libera/core/Expected.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/net/NetConfig.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamCommand.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/etherdream/EtherDreamControllerInfo.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

#define LIBERA_ENABLE_TEST_HOOKS 1
#include "libera/etherdream/EtherDreamController.hpp"
#undef LIBERA_ENABLE_TEST_HOOKS

#include "libera/log/Log.hpp"

using namespace std::chrono_literals;
using namespace libera;
using namespace libera::etherdream;

static int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { logError("ASSERT TRUE FAILED", (msg), "@", __FILE__, __LINE__); ++g_failures; } } while(0)

#define ASSERT_EQ(a,b,msg) \
    do { auto _va=(a); auto _vb=(b); if (!((_va)==(_vb))) { \
        logError("ASSERT EQ FAILED", (msg), "lhs", +_va, "rhs", +_vb, "@", __FILE__, __LINE__); \
        ++g_failures; \
    } } while(0)

namespace libera::etherdream {

class EtherDreamControllerTestAccess {
public:
    static void setStatus(EtherDreamController& controller,
                          LightEngineState lightEngineState,
                          PlaybackState playbackState,
                          std::uint16_t bufferFullness,
                          std::uint32_t reportedPointRate,
                          std::chrono::steady_clock::time_point receivedAt) {
        controller.lastKnownStatus.lightEngineState = lightEngineState;
        controller.lastKnownStatus.playbackState = playbackState;
        controller.lastKnownStatus.bufferFullness = bufferFullness;
        controller.lastKnownStatus.pointRate = reportedPointRate;
        controller.lastReceiveTime = receivedAt;
    }

    static int estimateBufferFullness(const EtherDreamController& controller) {
        return controller.estimateBufferFullness();
    }

    static void updatePlaybackRequirements(EtherDreamController& controller,
                                           const EtherDreamStatus& status) {
        controller.updatePlaybackRequirements(status);
    }

    static void syncPointRate(EtherDreamController& controller) {
        controller.syncPointRate();
    }

    static void setLastSentPointRate(EtherDreamController& controller,
                                     std::uint32_t pointRate) {
        controller.lastSentPointRate = pointRate;
    }

    static void setPendingRateChangeCount(EtherDreamController& controller,
                                          std::size_t count) {
        controller.pendingRateChangeCount = count;
    }

    static bool stopRequired(const EtherDreamController& controller) {
        return controller.stopRequired;
    }

    static bool prepareRequired(const EtherDreamController& controller) {
        return controller.prepareRequired;
    }

    static bool beginRequired(const EtherDreamController& controller) {
        return controller.beginRequired;
    }

    static std::size_t pendingRateChangeCount(const EtherDreamController& controller) {
        return controller.pendingRateChangeCount;
    }
};

} // namespace libera::etherdream

namespace {

std::unique_ptr<EtherDreamController> makeController() {
    auto controller = std::make_unique<EtherDreamController>(
        EtherDreamControllerInfo{
            "loopback",
            "Loopback",
            "127.0.0.1",
            config::ETHERDREAM_DAC_PORT_DEFAULT,
            4096,
            {},
            100000
        });
    controller->setPointRate(30000);
    return controller;
}

void setStatus(EtherDreamController& controller,
               PlaybackState playbackState,
               std::uint16_t bufferFullness,
               std::uint32_t reportedPointRate,
               std::chrono::milliseconds age) {
    EtherDreamControllerTestAccess::setStatus(controller,
                                             LightEngineState::Ready,
                                             playbackState,
                                             bufferFullness,
                                             reportedPointRate,
                                             std::chrono::steady_clock::now() - age);
}

void testPlayingProjectionUsesConfiguredPointRate() {
    auto controller = makeController();
    setStatus(*controller, PlaybackState::Playing, 4095, 108000000, 10ms);

    const int estimated = EtherDreamControllerTestAccess::estimateBufferFullness(*controller);

    ASSERT_TRUE(estimated > 3500,
                "bogus DAC status rate must not make a full FIFO look empty");
    ASSERT_TRUE(estimated < 4095,
                "playing projection should still account for elapsed time");
}

void testPreparedProjectionDoesNotDrainBuffer() {
    auto controller = makeController();
    setStatus(*controller, PlaybackState::Prepared, 3906, 108000000, 1s);

    const int estimated = EtherDreamControllerTestAccess::estimateBufferFullness(*controller);

    ASSERT_EQ(estimated, 3906,
              "non-playing Ether Dream states should use reported fullness without drain projection");
}

void testRequestedPointRateClampsToControllerMaximum() {
    auto controller = makeController();
    controller->setPointRate(108000000);

    ASSERT_EQ(controller->getPointRate(), static_cast<std::uint32_t>(100000),
              "local point-rate requests should clamp to the advertised Ether Dream maximum");
}

void testImplausibleReportedPointRateForcesReset() {
    auto controller = makeController();
    EtherDreamStatus status{};
    status.lightEngineState = LightEngineState::Ready;
    status.playbackState = PlaybackState::Playing;
    status.bufferFullness = 4095;
    status.pointRate = 108000000;

    EtherDreamControllerTestAccess::updatePlaybackRequirements(*controller, status);

    ASSERT_TRUE(EtherDreamControllerTestAccess::stopRequired(*controller),
                "implausible active point rate should force stop/re-prepare instead of more data");
    ASSERT_TRUE(!EtherDreamControllerTestAccess::prepareRequired(*controller),
                "implausible active point rate reset should not prepare until stop completes");
    ASSERT_TRUE(!EtherDreamControllerTestAccess::beginRequired(*controller),
                "implausible active point rate reset should not begin until stream is rebuilt");
}

void testPointRateChangeWhilePlayingSchedulesRestart() {
    auto controller = makeController();
    setStatus(*controller, PlaybackState::Playing, 1000, 30000, 0ms);
    EtherDreamControllerTestAccess::setLastSentPointRate(*controller, 30000);
    EtherDreamControllerTestAccess::setPendingRateChangeCount(*controller, 1);

    controller->setPointRate(20000);
    EtherDreamControllerTestAccess::syncPointRate(*controller);

    ASSERT_TRUE(EtherDreamControllerTestAccess::stopRequired(*controller),
                "active Ether Dream point-rate changes should restart playback");
    ASSERT_TRUE(!EtherDreamControllerTestAccess::prepareRequired(*controller),
                "rate-change restart should stop before preparing");
    ASSERT_TRUE(!EtherDreamControllerTestAccess::beginRequired(*controller),
                "rate-change restart should not begin until data is prepared again");
    ASSERT_EQ(EtherDreamControllerTestAccess::pendingRateChangeCount(*controller),
              static_cast<std::size_t>(0),
              "rate-change restart should discard queued q/control-bit changes");
}

void testPointRateChangeBeforeBeginWaitsForBeginRate() {
    auto controller = makeController();
    setStatus(*controller, PlaybackState::Prepared, 1000, 0, 0ms);
    EtherDreamControllerTestAccess::setLastSentPointRate(*controller, 0);

    controller->setPointRate(20000);
    EtherDreamControllerTestAccess::syncPointRate(*controller);

    ASSERT_TRUE(!EtherDreamControllerTestAccess::stopRequired(*controller),
                "before playback starts, begin command will carry the requested rate");
}

} // namespace

int main() {
    testPlayingProjectionUsesConfiguredPointRate();
    testPreparedProjectionDoesNotDrainBuffer();
    testRequestedPointRateClampsToControllerMaximum();
    testImplausibleReportedPointRateForcesReset();
    testPointRateChangeWhilePlayingSchedulesRestart();
    testPointRateChangeBeforeBeginWaitsForBeginRate();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("EtherDream buffer projection tests passed");
    return 0;
}
