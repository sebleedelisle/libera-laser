#include "libera/helios/HeliosController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace libera::helios {
namespace {

constexpr std::size_t DEFAULT_FRAME_POINTS = 1000; // maximum 4096 points
constexpr std::size_t MIN_FRAME_POINTS = 20;

constexpr unsigned int HELIOS_FLAGS = HELIOS_FLAGS_DEFAULT;

} // namespace

namespace error_types = libera::core::error_types;

HeliosController::HeliosController(std::shared_ptr<HeliosDac> sdkInstance, unsigned int controllerIndex)
: sdk(std::move(sdkInstance))
, index(controllerIndex) {
    targetFramePoints.store(DEFAULT_FRAME_POINTS, std::memory_order_relaxed);
    frameBuffer.reserve(DEFAULT_FRAME_POINTS);
    setEstimatedBufferCapacity(static_cast<int>(DEFAULT_FRAME_POINTS));
    updateEstimatedBufferAnchorNow(0, getPointRate());
}

HeliosController::~HeliosController() {
    stop();
    close();
}

void HeliosController::close() {
    setConnectionState(false);
    clearEstimatedBufferState();
    if (!sdk) {
        return;
    }
    sdk->Stop(index);
}

bool HeliosController::isConnected() const {
    if (!sdk) {
        return false;
    }
    return sdk->GetIsClosed(index) == 0;
}

void HeliosController::setPointRate(std::uint32_t pointRateValue) {
    LaserControllerStreaming::setPointRate(pointRateValue);
}

void HeliosController::setFramePointCount(std::size_t points) {
    const auto clamped = std::max(points, MIN_FRAME_POINTS);
    targetFramePoints.store(clamped, std::memory_order_relaxed);
    frameBuffer.reserve(clamped);
    setEstimatedBufferCapacity(static_cast<int>(clamped));
}

std::size_t HeliosController::framePointCount() const {
    return targetFramePoints.load(std::memory_order_relaxed);
}

void HeliosController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();
    bool wasConnected = false;

    while (running) {
        if (!sdk) {
            if (wasConnected) {
                recordConnectionError(error_types::usb::connectionLost);
            }
            setConnectionState(false);
            wasConnected = false;
            std::this_thread::sleep_for(100ms);
            continue;
        }

        if (sdk->GetIsClosed(index) != 0) {
            if (wasConnected) {
                recordConnectionError(error_types::usb::connectionLost);
            }
            setConnectionState(false);
            wasConnected = false;
            std::this_thread::sleep_for(100ms);
            continue;
        }
        setConnectionState(true);
        wasConnected = true;

        const int status = sdk->GetStatus(index);
        if (status < 0) {
            // -5007 is a libusb timeout from status polling. Treat as transient.
            if (status == -5007) {
                recordIntermittentError(error_types::usb::timeout);
                std::this_thread::sleep_for(2ms);
                continue;
            }
            logError("[HeliosController] status error", status);
            recordIntermittentError(error_types::usb::statusError);
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (status == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        const std::size_t framePoints = targetFramePoints.load(std::memory_order_relaxed);
        const unsigned int pps = getPointRate();

        // SDK status says the DAC is ready, so treat current queued fullness as empty.
        setEstimatedBufferCapacity(static_cast<int>(framePoints));
        updateEstimatedBufferAnchorNow(0, pps);

        core::PointFillRequest req;
        req.minimumPointsRequired = framePoints;
        req.maximumPointsRequired = framePoints;
        req.estimatedFirstPointRenderTime =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<int>(pointsToMillis(framePoints)));
        req.currentPointIndex = currentPointIndex.load(std::memory_order_relaxed);

        if (!requestPoints(req)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (pointsToSend.empty()) {
            continue;
        }

        frameBuffer.resize(pointsToSend.size());
        for (std::size_t i = 0; i < pointsToSend.size(); ++i) {
            const auto& p = pointsToSend[i];
            auto& out = frameBuffer[i];
            out.x = encodeUnsigned16FromSignedUnit(p.x);
            out.y = encodeUnsigned16FromSignedUnit(p.y);
            out.r = encodeUnsigned16FromUnit(p.r);
            out.g = encodeUnsigned16FromUnit(p.g);
            out.b = encodeUnsigned16FromUnit(p.b);
            out.i = encodeUnsigned16FromUnit(p.i);
            out.user1 = encodeUnsigned16FromUnit(p.u1);
            out.user2 = encodeUnsigned16FromUnit(p.u2);
            out.user3 = 0;
            out.user4 = 0;
        }

        const auto sendStart = std::chrono::steady_clock::now();
        const int result = sdk->WriteFrameExtended(
            index,
            pps,
            HELIOS_FLAGS,
            frameBuffer.data(),
            static_cast<unsigned int>(frameBuffer.size()));
        const auto sendDone = std::chrono::steady_clock::now();

        if (result < 0) {
            logError("[HeliosController] WriteFrameExtended failed", result);
            if (result == -5007) {
                recordIntermittentError(error_types::usb::timeout);
            } else {
                recordIntermittentError(error_types::usb::transferFailed);
            }
        } else {
            recordLatencySample(sendDone - sendStart);
            setEstimatedBufferCapacity(static_cast<int>(frameBuffer.size()));
            updateEstimatedBufferAnchor(
                static_cast<int>(frameBuffer.size()),
                sendDone,
                pps);
            currentPointIndex.fetch_add(frameBuffer.size(), std::memory_order_relaxed);
        }
    }
}

} // namespace libera::helios
