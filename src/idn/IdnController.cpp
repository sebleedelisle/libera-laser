#include "libera/idn/IdnController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/helios/HeliosTransportSupport.hpp"
#include "libera/log/Log.hpp"

#include <cstring>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // Keep Windows headers from defining min/max macros that break std::min/std::max.
#endif
#define _WINSOCKAPI_
#endif
#include "libusb.h"

namespace libera::idn {
namespace {

std::string describeHeliosError(int code) {
    // Helios SDK error values are shared across USB and IDN. Preserve the same
    // textual mapping here so the log output remains comparable after the
    // transport split.
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
        return "null_points";
    case HELIOS_ERROR_DEVICE_SIGNAL_TOO_LONG:
        return "signal_too_long";
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

} // namespace

namespace error_types = libera::core::error_types;

IdnController::IdnController(std::shared_ptr<HeliosDac> sdkInstance, unsigned int controllerIndex)
    : sdk(std::move(sdkInstance))
    , index(controllerIndex) {
    const auto defaultFramePoints = helios::detail::defaultFramePointCount(getPointRate());
    targetFramePoints.store(defaultFramePoints, std::memory_order_relaxed);
    frameBuffer.reserve(defaultFramePoints);
    setEstimatedBufferCapacity(static_cast<int>(defaultFramePoints));
    updateEstimatedBufferSnapshotNow(0, getPointRate());
    statusWarmupDeadline = std::chrono::steady_clock::now() + helios::detail::STATUS_ERROR_WARMUP_GRACE;
}

IdnController::~IdnController() {
    stopThread();
    close();
}

void IdnController::close() {
    setConnectionState(false);
    clearFrameTransportSubmissionEstimate();
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
    if (sdk) {
        sdk->Stop(index.load(std::memory_order_relaxed));
    }
}

bool IdnController::isConnected() const {
    return sdk && sdk->GetIsClosed(index.load(std::memory_order_relaxed)) == 0;
}

void IdnController::updateControllerIndex(unsigned int controllerIndex) {
    if (!sdk) {
        return;
    }

    const unsigned int previousIndex =
        index.exchange(controllerIndex, std::memory_order_relaxed);
    if (previousIndex == controllerIndex) {
        return;
    }

    // An IDN reconnect can reassign the stable unit ID to a different transient
    // SDK slot. Flush short-term pacing state so the stream restarts cleanly on
    // the replacement slot instead of carrying stale timing assumptions across.
    consecutiveStatusErrors = 0;
    consecutiveWriteErrors = 0;
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
    clearFrameTransportSubmissionEstimate();
    statusWarmupDeadline =
        std::chrono::steady_clock::now() + helios::detail::STATUS_ERROR_WARMUP_GRACE;
    resetStartupBlank();
}

void IdnController::setPointRate(std::uint32_t pointRateValue) {
    LaserControllerStreaming::setPointRate(pointRateValue);
    if (!framePointCountExplicitlySet.load(std::memory_order_relaxed)) {
        const auto automaticFramePoints =
            helios::detail::defaultFramePointCount(pointRateValue);
        targetFramePoints.store(automaticFramePoints, std::memory_order_relaxed);
        frameBuffer.reserve(automaticFramePoints);
        setEstimatedBufferCapacity(static_cast<int>(automaticFramePoints));
    }
}

void IdnController::setFramePointCount(std::size_t points) {
    const auto clamped = std::clamp<std::size_t>(points,
                                                 helios::detail::MIN_FRAME_POINTS,
                                                 HELIOS_MAX_POINTS);
    framePointCountExplicitlySet.store(true, std::memory_order_relaxed);
    targetFramePoints.store(clamped, std::memory_order_relaxed);
    frameBuffer.reserve(clamped);
    setEstimatedBufferCapacity(static_cast<int>(clamped));
}

std::size_t IdnController::framePointCount() const {
    return targetFramePoints.load(std::memory_order_relaxed);
}

int IdnController::getFirmwareVersion() const {
    if (!sdk) {
        return 0;
    }
    return sdk->GetFirmwareVersion(index.load(std::memory_order_relaxed));
}

std::string IdnController::getDacName() const {
    char buf[32] = {0};
    if (sdk) {
        sdk->GetName(index.load(std::memory_order_relaxed), buf);
    }
    return std::string(buf);
}

bool IdnController::setDacName(const std::string& name) {
    if (!sdk) {
        return false;
    }

    std::string truncated = name.substr(0, 30);
    char buf[32] = {0};
    std::strncpy(buf, truncated.c_str(), 30);
    return sdk->SetName(index.load(std::memory_order_relaxed), buf) == HELIOS_SUCCESS;
}

void IdnController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();
    bool wasConnected = false;

    while (running) {
        const unsigned int sdkIndex = index.load(std::memory_order_relaxed);
        const bool backendConnected = sdk && (sdk->GetIsClosed(sdkIndex) == 0);
        if (!backendConnected) {
            if (wasConnected) {
                recordConnectionError(error_types::usb::connectionLost);
            }
            setConnectionState(false);
            clearFrameTransportSubmissionEstimate();
            wasConnected = false;
            std::this_thread::sleep_for(100ms);
            continue;
        }

        setConnectionState(true);
        if (!wasConnected) {
            resetStartupBlank();
            statusWarmupDeadline =
                std::chrono::steady_clock::now() + helios::detail::STATUS_ERROR_WARMUP_GRACE;
        }
        wasConnected = true;

        const int status = sdk->GetStatus(sdkIndex);
        if (status < 0) {
            if (std::chrono::steady_clock::now() < statusWarmupDeadline) {
                consecutiveStatusErrors = 0;
                std::this_thread::sleep_for(2ms);
                continue;
            }
            if (status == -5007) {
                recordIntermittentError(error_types::usb::timeout);
                std::this_thread::sleep_for(2ms);
                continue;
            }
            recordIntermittentError(error_types::usb::statusError);
            ++consecutiveStatusErrors;
            if (helios::detail::shouldLogErrorBurst(consecutiveStatusErrors)) {
                logError("[IdnController] status error",
                         "index", std::to_string(sdkIndex),
                         "code", status,
                         "reason", describeHeliosError(status),
                         "consecutive", consecutiveStatusErrors);
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        statusWarmupDeadline = std::chrono::steady_clock::time_point{};
        consecutiveStatusErrors = 0;

        if (status == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        const std::size_t framePoints = targetFramePoints.load(std::memory_order_relaxed);
        const unsigned int pps = getPointRate();
        const auto writeLead = helios::detail::requestRenderLead(
            std::chrono::microseconds(
                estimatedWriteLeadMicros.load(std::memory_order_relaxed)));
        const auto estimatedFirstRenderTime =
            std::chrono::steady_clock::now() + writeLead;
        const auto pointIndex = currentPointIndex.load(std::memory_order_relaxed);

        setEstimatedBufferCapacity(static_cast<int>(framePoints));
        updateEstimatedBufferSnapshotNow(0, pps);

        // The SDK-backed path still behaves as a point-ingester from the shared
        // scheduler's perspective: pull one controller-sized point batch, then
        // hand that batch to the SDK for packing and submission.
        core::PointFillRequest req;
        req.minimumPointsRequired = framePoints;
        req.maximumPointsRequired = framePoints;
        req.estimatedFirstPointRenderTime = estimatedFirstRenderTime;
        req.currentPointIndex = pointIndex;

        if (!requestPoints(req)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (pointsToSend.empty()) {
            continue;
        }

        helios::detail::encodeFramePoints(pointsToSend, frameBuffer);

        const auto sendStart = std::chrono::steady_clock::now();
        const int result = sdk->WriteFrameExtended(sdkIndex,
                                                   pps,
                                                   helios::detail::HELIOS_FLAGS,
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
            if (helios::detail::shouldLogErrorBurst(consecutiveWriteErrors)) {
                logError("[IdnController] WriteFrameExtended failed",
                         "index", std::to_string(sdkIndex),
                         "code", result,
                         "reason", describeHeliosError(result),
                         "consecutive", consecutiveWriteErrors,
                         "point_count", frameBuffer.size(),
                         "pps", pps);
            }
            continue;
        }

        consecutiveWriteErrors = 0;
        recordLatencySample(sendDone - sendStart);
        const auto measuredWriteLeadMicros =
            std::chrono::duration_cast<std::chrono::microseconds>(sendDone - sendStart).count();
        const auto previousWriteLeadMicros =
            estimatedWriteLeadMicros.load(std::memory_order_relaxed);
        estimatedWriteLeadMicros.store(
            helios::detail::smoothWriteLeadMicros(previousWriteLeadMicros,
                                                  measuredWriteLeadMicros),
            std::memory_order_relaxed);
        setEstimatedBufferCapacity(static_cast<int>(frameBuffer.size()));
        updateEstimatedBufferSnapshot(static_cast<int>(frameBuffer.size()),
                                     sendDone,
                                     pps);
        currentPointIndex.fetch_add(frameBuffer.size(), std::memory_order_relaxed);
    }
}

} // namespace libera::idn
