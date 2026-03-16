#include "libera/helios/HeliosController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <string>
#include <thread>

namespace libera::helios {
namespace {

constexpr std::size_t MIN_FRAME_POINTS = 20;
constexpr double TARGET_FRAME_DURATION_MS = 10.0;

constexpr unsigned int HELIOS_FLAGS = HELIOS_FLAGS_DEFAULT;

std::string describeHeliosError(int code) {
    // libusb errors are encoded as HELIOS_ERROR_LIBUSB_BASE + libusb_error.
    if (code <= HELIOS_ERROR_LIBUSB_BASE) {
        const int libusbCode = code - HELIOS_ERROR_LIBUSB_BASE;
        return std::string("libusb:") + libusb_error_name(libusbCode);
    }

    switch (code) {
    case HELIOS_ERROR_DEVICE_CLOSED:
        return "device_closed";
    case HELIOS_ERROR_DEVICE_FRAME_READY:
        return "device_frame_ready";
    case HELIOS_ERROR_DEVICE_SEND_CONTROL:
        return "send_control_failed";
    case HELIOS_ERROR_DEVICE_RESULT:
        return "device_result_unexpected";
    case HELIOS_ERROR_DEVICE_NULL_BUFFER:
        return "device_null_buffer";
    case HELIOS_ERROR_DEVICE_SIGNAL_TOO_LONG:
        return "device_signal_too_long";
    case HELIOS_ERROR_NOT_INITIALIZED:
        return "not_initialized";
    case HELIOS_ERROR_INVALID_DEVNUM:
        return "invalid_device_index";
    case HELIOS_ERROR_NULL_POINTS:
        return "null_points";
    case HELIOS_ERROR_TOO_MANY_POINTS:
        return "too_many_points";
    case HELIOS_ERROR_PPS_TOO_HIGH:
        return "pps_too_high";
    case HELIOS_ERROR_PPS_TOO_LOW:
        return "pps_too_low";
    default:
        return "unknown";
    }
}

bool shouldLogErrorBurst(std::size_t consecutiveCount) {
    // Always log first failure, then throttle to keep console readable.
    return consecutiveCount == 1 || (consecutiveCount % 25 == 0);
}

} // namespace

namespace error_types = libera::core::error_types;

std::size_t detail::defaultFramePointCount(std::uint32_t pointRate) {
    if (pointRate == 0) {
        return MIN_FRAME_POINTS;
    }
    const double rawPoints =
        (static_cast<double>(pointRate) * TARGET_FRAME_DURATION_MS) / 1000.0;
    const auto roundedPoints = static_cast<std::size_t>(std::llround(rawPoints));
    return std::clamp<std::size_t>(roundedPoints, MIN_FRAME_POINTS, HELIOS_MAX_POINTS);
}

std::size_t detail::minimumRequestPoints(std::size_t maxFramePoints) {
    return std::min<std::size_t>(maxFramePoints, MIN_FRAME_POINTS);
}

std::chrono::steady_clock::duration detail::requestRenderLead(std::chrono::microseconds previousWriteLead) {
    return std::chrono::microseconds(std::max<std::int64_t>(0, previousWriteLead.count()));
}

std::int64_t detail::smoothWriteLeadMicros(std::int64_t previousMicros, std::int64_t currentMicros) {
    const auto clampedPrevious = std::max<std::int64_t>(0, previousMicros);
    const auto clampedCurrent = std::max<std::int64_t>(0, currentMicros);
    if (clampedPrevious == 0) {
        return clampedCurrent;
    }
    return ((clampedPrevious * 3) + clampedCurrent) / 4;
}

HeliosController::HeliosController(std::shared_ptr<HeliosDac> sdkInstance, unsigned int controllerIndex)
: sdk(std::move(sdkInstance))
, index(controllerIndex) {
    const auto defaultFramePoints = detail::defaultFramePointCount(getPointRate());
    targetFramePoints.store(defaultFramePoints, std::memory_order_relaxed);
    frameBuffer.reserve(defaultFramePoints);
    setEstimatedBufferCapacity(static_cast<int>(defaultFramePoints));
    updateEstimatedBufferAnchorNow(0, getPointRate());
}

HeliosController::~HeliosController() {
    stop();
    close();
}

void HeliosController::close() {
    setConnectionState(false);
    clearEstimatedBufferState();
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
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
    if (!framePointCountExplicitlySet.load(std::memory_order_relaxed)) {
        const auto automaticFramePoints = detail::defaultFramePointCount(pointRateValue);
        targetFramePoints.store(automaticFramePoints, std::memory_order_relaxed);
        frameBuffer.reserve(automaticFramePoints);
        setEstimatedBufferCapacity(static_cast<int>(automaticFramePoints));
    }
}

void HeliosController::setFramePointCount(std::size_t points) {
    const auto clamped = std::clamp<std::size_t>(points, MIN_FRAME_POINTS, HELIOS_MAX_POINTS);
    framePointCountExplicitlySet.store(true, std::memory_order_relaxed);
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
            recordIntermittentError(error_types::usb::statusError);
            ++consecutiveStatusErrors;
            if (shouldLogErrorBurst(consecutiveStatusErrors)) {
                logError("[HeliosController] status error",
                         "index", index,
                         "code", status,
                         "reason", describeHeliosError(status),
                         "consecutive", consecutiveStatusErrors,
                         "is_closed", sdk->GetIsClosed(index));
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        consecutiveStatusErrors = 0;

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
        req.minimumPointsRequired = detail::minimumRequestPoints(framePoints);
        req.maximumPointsRequired = framePoints;
        // Helios only exposes ready/not-ready, not continuous buffer depth.
        // Once GetStatus() says "ready", the best lead we have is the USB/SDK
        // write duration we observed on recent successful transfers, not one
        // whole frame of extra delay.
        const auto writeLead = detail::requestRenderLead(
            std::chrono::microseconds(
                estimatedWriteLeadMicros.load(std::memory_order_relaxed)));
        req.estimatedFirstPointRenderTime =
            std::chrono::steady_clock::now() + writeLead;
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
            // Some legacy controllers still use the dedicated intensity word.
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
            if (result == -5007) {
                recordIntermittentError(error_types::usb::timeout);
            } else {
                recordIntermittentError(error_types::usb::transferFailed);
            }
            ++consecutiveWriteErrors;
            if (shouldLogErrorBurst(consecutiveWriteErrors)) {
                logError("[HeliosController] WriteFrameExtended failed",
                         "index", index,
                         "code", result,
                         "reason", describeHeliosError(result),
                         "consecutive", consecutiveWriteErrors,
                         "point_count", frameBuffer.size(),
                         "pps", pps);
            }
        } else {
            consecutiveWriteErrors = 0;
            recordLatencySample(sendDone - sendStart);
            const auto measuredWriteLeadMicros =
                std::chrono::duration_cast<std::chrono::microseconds>(sendDone - sendStart).count();
            const auto previousWriteLeadMicros =
                estimatedWriteLeadMicros.load(std::memory_order_relaxed);
            estimatedWriteLeadMicros.store(
                detail::smoothWriteLeadMicros(previousWriteLeadMicros, measuredWriteLeadMicros),
                std::memory_order_relaxed);
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
