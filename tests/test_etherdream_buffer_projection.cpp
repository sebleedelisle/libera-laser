#include "libera/core/Expected.hpp"
#include "libera/core/ControllerErrorTypes.hpp"
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
#include <string>
#include <string_view>
#include <vector>

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

    static core::PointFillRequest getFillRequest(EtherDreamController& controller) {
        return controller.getFillRequest();
    }

    static bool shouldRequestPoints(const EtherDreamController& controller,
                                    const core::PointFillRequest& request) {
        return controller.shouldRequestPoints(request);
    }

    static std::size_t pendingRateChangeCount(const EtherDreamController& controller) {
        return controller.pendingRateChangeCount;
    }

    static std::size_t playingUnderrunBufferThreshold(const EtherDreamController& controller) {
        return controller.playingUnderrunBufferThreshold();
    }

    static bool statusReportsPlayingBufferUnderrun(const EtherDreamController& controller,
                                                   const EtherDreamStatus& status) {
        return controller.statusReportsPlayingBufferUnderrun(status);
    }

    static void setPendingStreamHealthRequest(
        EtherDreamController& controller,
        bool playbackWasPlaying,
        int estimatedBufferBeforeRequest,
        int targetBufferPointCount,
        std::chrono::steady_clock::duration requestDuration) {
        controller.pendingStreamHealthRequest = EtherDreamController::PendingStreamHealthRequest{};
        controller.pendingStreamHealthRequest.valid = true;
        controller.pendingStreamHealthRequest.playbackWasPlaying = playbackWasPlaying;
        controller.pendingStreamHealthRequest.estimatedBufferBeforeRequest =
            estimatedBufferBeforeRequest;
        controller.pendingStreamHealthRequest.targetBufferPointCount =
            targetBufferPointCount;
        controller.pendingStreamHealthRequest.requestDuration = requestDuration;
    }

    static bool pendingStreamHealthRequestLikelyStarvedDac(
        const EtherDreamController& controller) {
        return controller.pendingStreamHealthRequestLikelyStarvedDac();
    }

    static void setLitPointsToSend(EtherDreamController& controller, std::size_t pointCount) {
        controller.pointsToSend.clear();
        controller.pointsToSend.reserve(pointCount);
        for (std::size_t i = 0; i < pointCount; ++i) {
            core::LaserPoint point{};
            point.x = static_cast<float>(i) * 0.001f;
            point.r = 1.0f;
            point.g = 1.0f;
            point.b = 1.0f;
            controller.pointsToSend.push_back(point);
        }
    }

    static void applyUnderrunRecoveryBlankToCurrentPacket(
        EtherDreamController& controller) {
        controller.applyUnderrunRecoveryBlankToCurrentPacket();
    }

    static bool dataPacketWouldOverflowBuffer(EtherDreamController& controller,
                                              std::uint16_t pointCount,
                                              std::uint16_t reportedBufferFullness) {
        controller.protocolTxHistory = {};
        controller.nextProtocolTxHistoryIndex = 0;
        controller.commandBuffer.setDataCommand(pointCount);
        controller.recordProtocolTx(42, 'd');

        EtherDreamStatus status{};
        status.lightEngineState = LightEngineState::Ready;
        status.playbackState = PlaybackState::Playing;
        status.bufferFullness = reportedBufferFullness;
        return controller.dataPacketWouldOverflowBuffer(status, 42);
    }

    static void recordStreamHealthPacket(
        EtherDreamController& controller,
        const EtherDreamStatus& ackStatus,
        std::chrono::steady_clock::duration sendDuration) {
        controller.recordStreamHealthPacket(ackStatus, sendDuration);
    }

    static void recordBufferOverrun(EtherDreamController& controller) {
        controller.recordBufferOverrun();
    }

    static void applyStartupBlankToOutputPoints(
        EtherDreamController& controller,
        std::vector<core::LaserPoint>& points) {
        controller.applyStartupBlankToOutputPoints(points);
    }

    static const std::vector<core::LaserPoint>& pointsToSend(
        const EtherDreamController& controller) {
        return controller.pointsToSend;
    }
};

} // namespace libera::etherdream

namespace {

std::unique_ptr<EtherDreamController> makeController(std::uint16_t hardwareRevision = 30) {
    auto controller = std::make_unique<EtherDreamController>(
        EtherDreamControllerInfo{
            "loopback",
            "Loopback",
            "127.0.0.1",
            config::ETHERDREAM_DAC_PORT_DEFAULT,
            4096,
            {},
            100000,
            hardwareRevision
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

void testFillRequestLeavesOnePointWriteHeadroom() {
    auto controller = makeController(30);
    setStatus(*controller, PlaybackState::Prepared, 3946, 0, 0ms);

    const auto request = EtherDreamControllerTestAccess::getFillRequest(*controller);

    ASSERT_EQ(request.maximumPointsRequired, static_cast<std::size_t>(149),
              "Ether Dream writes should leave one point free instead of exactly filling the FIFO");
    ASSERT_TRUE(request.minimumPointsRequired <= request.maximumPointsRequired,
                "minimum request must stay within capped packet space");

    setStatus(*controller, PlaybackState::Prepared, 4095, 0, 0ms);
    const auto nearlyFullRequest = EtherDreamControllerTestAccess::getFillRequest(*controller);
    ASSERT_EQ(nearlyFullRequest.maximumPointsRequired, static_cast<std::size_t>(0),
              "one advertised free point is reserved as write headroom");
}

void testDataNakWithPartialFullStatusCanBeBufferOverrun() {
    auto controller = makeController(30);

    ASSERT_TRUE(EtherDreamControllerTestAccess::dataPacketWouldOverflowBuffer(
                    *controller,
                    596,
                    3500),
                "packet that exactly reaches advertised capacity should be treated as overrun when rejected");
    ASSERT_TRUE(!EtherDreamControllerTestAccess::dataPacketWouldOverflowBuffer(
                    *controller,
                    595,
                    3500),
                "packet that leaves configured write headroom should still fit");
}

void testOlderEtherDreamOnlyTreatsZeroPlayingBufferAsUnderrun() {
    auto controller = makeController(10);
    ASSERT_EQ(EtherDreamControllerTestAccess::playingUnderrunBufferThreshold(*controller),
              static_cast<std::size_t>(1),
              "older Ether Dream underrun threshold");

    EtherDreamStatus status{};
    status.lightEngineState = LightEngineState::Ready;
    status.playbackState = PlaybackState::Playing;
    status.bufferFullness = 0;
    ASSERT_TRUE(EtherDreamControllerTestAccess::statusReportsPlayingBufferUnderrun(*controller, status),
                "older Ether Dream playing with zero buffer is underrun");

    status.bufferFullness = 1;
    ASSERT_TRUE(!EtherDreamControllerTestAccess::statusReportsPlayingBufferUnderrun(*controller, status),
                "older Ether Dream playing with one point is not treated as underrun");
}

void testNewerEtherDreamTreatsSub256PlayingBufferAsUnderrun() {
    auto controller = makeController(30);
    ASSERT_EQ(EtherDreamControllerTestAccess::playingUnderrunBufferThreshold(*controller),
              config::ETHERDREAM_MIN_BUFFER_POINTS,
              "newer Ether Dream underrun threshold");

    EtherDreamStatus status{};
    status.lightEngineState = LightEngineState::Ready;
    status.playbackState = PlaybackState::Playing;
    status.bufferFullness = static_cast<std::uint16_t>(config::ETHERDREAM_MIN_BUFFER_POINTS - 1);
    ASSERT_TRUE(EtherDreamControllerTestAccess::statusReportsPlayingBufferUnderrun(*controller, status),
                "newer Ether Dream playing below 256 points is underrun");

    status.bufferFullness = static_cast<std::uint16_t>(config::ETHERDREAM_MIN_BUFFER_POINTS);
    ASSERT_TRUE(!EtherDreamControllerTestAccess::statusReportsPlayingBufferUnderrun(*controller, status),
                "newer Ether Dream playing at 256 points is still above underrun threshold");

    status.playbackState = PlaybackState::Prepared;
    status.bufferFullness = 0;
    ASSERT_TRUE(!EtherDreamControllerTestAccess::statusReportsPlayingBufferUnderrun(*controller, status),
                "prepared low buffer should not be treated as playing-mode underrun");
}

void testEstimatedPlayingBufferBelowThresholdCountsAsComputerUnderrun() {
    auto olderController = makeController(10);
    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *olderController,
        true,
        1,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);
    ASSERT_TRUE(!EtherDreamControllerTestAccess::pendingStreamHealthRequestLikelyStarvedDac(
                    *olderController),
                "older Ether Dream estimated one-point playing buffer is not a forced underrun");

    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *olderController,
        true,
        0,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);
    ASSERT_TRUE(EtherDreamControllerTestAccess::pendingStreamHealthRequestLikelyStarvedDac(
                    *olderController),
                "older Ether Dream estimated zero playing buffer is an underrun");

    auto newerController = makeController(30);
    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *newerController,
        true,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);
    ASSERT_TRUE(!EtherDreamControllerTestAccess::pendingStreamHealthRequestLikelyStarvedDac(
                    *newerController),
                "newer Ether Dream estimated 256-point playing buffer is not an underrun");

    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *newerController,
        true,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS - 1),
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);
    ASSERT_TRUE(EtherDreamControllerTestAccess::pendingStreamHealthRequestLikelyStarvedDac(
                    *newerController),
                "newer Ether Dream estimated sub-256 playing buffer is an underrun");

    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *newerController,
        false,
        0,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);
    ASSERT_TRUE(!EtherDreamControllerTestAccess::pendingStreamHealthRequestLikelyStarvedDac(
                    *newerController),
                "non-playing state should not be classified as computer underrun");
}

void testSlowPointRequestCanCountAsComputerUnderrun() {
    auto controller = makeController(30);
    const int estimatedBuffer = 300;

    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *controller,
        true,
        estimatedBuffer,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        1ms);
    ASSERT_TRUE(!EtherDreamControllerTestAccess::pendingStreamHealthRequestLikelyStarvedDac(
                    *controller),
                "request shorter than time to underrun threshold should not count as underrun");

    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *controller,
        true,
        estimatedBuffer,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        2ms);
    ASSERT_TRUE(EtherDreamControllerTestAccess::pendingStreamHealthRequestLikelyStarvedDac(
                    *controller),
                "request long enough to drain below threshold should count as underrun");
}

void testThresholdCrossingComputerUnderrunIsReported() {
    auto controller = makeController(30);
    controller->clearErrors();
    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *controller,
        true,
        300,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        2ms);

    EtherDreamStatus ackStatus{};
    ackStatus.lightEngineState = LightEngineState::Ready;
    ackStatus.playbackState = PlaybackState::Playing;
    ackStatus.bufferFullness = 1000;
    ackStatus.pointRate = 30000;
    EtherDreamControllerTestAccess::recordStreamHealthPacket(*controller, ackStatus, 0ms);

    bool foundComputerUnderrun = false;
    const std::string streamStarvationCode(
        core::error_types::etherdream::streamStarvation);
    for (const auto& error : controller->getErrors()) {
        if (error.code == streamStarvationCode &&
            error.count == 1) {
            foundComputerUnderrun = true;
            break;
        }
    }
    ASSERT_TRUE(foundComputerUnderrun,
                "crossing the playing underrun threshold should be reported as computer underrun");
}

void testPreSendUnderrunAppliesStartupBlankToCurrentPacket() {
    auto controller = makeController(30);
    controller->setPointRate(30000);
    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *controller,
        true,
        0,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);
    EtherDreamControllerTestAccess::setLitPointsToSend(*controller, 40);

    EtherDreamControllerTestAccess::applyUnderrunRecoveryBlankToCurrentPacket(*controller);

    const auto& points = EtherDreamControllerTestAccess::pointsToSend(*controller);
    ASSERT_EQ(points.size(), static_cast<std::size_t>(40),
              "recovery blanking should not resize current packet");
    for (std::size_t i = 0; i < 30; ++i) {
        ASSERT_EQ(points[i].r, 0.0f, "startup blank red");
        ASSERT_EQ(points[i].g, 0.0f, "startup blank green");
        ASSERT_EQ(points[i].b, 0.0f, "startup blank blue");
    }
    ASSERT_EQ(points[30].r, 1.0f, "point after 1 ms startup blank remains lit");
    ASSERT_EQ(points[30].g, 1.0f, "point after 1 ms startup blank remains lit");
    ASSERT_EQ(points[30].b, 1.0f, "point after 1 ms startup blank remains lit");
}

void testBufferOverrunArmsStartupBlankForNextPacket() {
    auto controller = makeController(30);
    controller->setPointRate(30000);

    EtherDreamControllerTestAccess::recordBufferOverrun(*controller);

    std::vector<core::LaserPoint> points;
    points.reserve(40);
    for (std::size_t i = 0; i < 40; ++i) {
        core::LaserPoint point{};
        point.r = 1.0f;
        point.g = 1.0f;
        point.b = 1.0f;
        points.push_back(point);
    }
    EtherDreamControllerTestAccess::applyStartupBlankToOutputPoints(*controller, points);

    for (std::size_t i = 0; i < 30; ++i) {
        ASSERT_EQ(points[i].r, 0.0f, "overrun recovery blank red");
        ASSERT_EQ(points[i].g, 0.0f, "overrun recovery blank green");
        ASSERT_EQ(points[i].b, 0.0f, "overrun recovery blank blue");
    }
    ASSERT_EQ(points[30].r, 1.0f, "point after overrun startup blank remains lit");
    ASSERT_EQ(points[30].g, 1.0f, "point after overrun startup blank remains lit");
    ASSERT_EQ(points[30].b, 1.0f, "point after overrun startup blank remains lit");
}

void testPlayingBelowUnderrunThresholdRequestsPointsImmediately() {
    auto controller = makeController(30);
    setStatus(*controller,
              PlaybackState::Playing,
              static_cast<std::uint16_t>(config::ETHERDREAM_MIN_BUFFER_POINTS - 1),
              30000,
              0ms);
    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *controller,
        true,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS - 1),
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);

    core::PointFillRequest request{};
    request.minimumPointsRequired = 1;
    request.maximumPointsRequired = 300;

    ASSERT_TRUE(EtherDreamControllerTestAccess::shouldRequestPoints(*controller, request),
                "playing below ED3+ threshold should request points even for a small deficit");

    EtherDreamControllerTestAccess::setPendingStreamHealthRequest(
        *controller,
        true,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        0ms);
    ASSERT_TRUE(!EtherDreamControllerTestAccess::shouldRequestPoints(*controller, request),
                "playing at threshold should keep the normal minimum packet gate");
}

} // namespace

int main() {
    testPlayingProjectionUsesConfiguredPointRate();
    testPreparedProjectionDoesNotDrainBuffer();
    testRequestedPointRateClampsToControllerMaximum();
    testImplausibleReportedPointRateForcesReset();
    testPointRateChangeWhilePlayingSchedulesRestart();
    testPointRateChangeBeforeBeginWaitsForBeginRate();
    testFillRequestLeavesOnePointWriteHeadroom();
    testDataNakWithPartialFullStatusCanBeBufferOverrun();
    testOlderEtherDreamOnlyTreatsZeroPlayingBufferAsUnderrun();
    testNewerEtherDreamTreatsSub256PlayingBufferAsUnderrun();
    testEstimatedPlayingBufferBelowThresholdCountsAsComputerUnderrun();
    testSlowPointRequestCanCountAsComputerUnderrun();
    testThresholdCrossingComputerUnderrunIsReported();
    testPreSendUnderrunAppliesStartupBlankToCurrentPacket();
    testBufferOverrunArmsStartupBlankForNextPacket();
    testPlayingBelowUnderrunThresholdRequestsPointsImmediately();

    if (g_failures) {
        logError("Tests failed", g_failures, "failure(s)");
        return 1;
    }

    logInfo("EtherDream buffer projection tests passed");
    return 0;
}
