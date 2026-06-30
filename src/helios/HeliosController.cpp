#include "libera/helios/HeliosController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/helios/HeliosTransportSupport.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // Keep Windows headers from defining min/max macros that break std::min/std::max.
#endif
#define _WINSOCKAPI_
#endif
#include "libusb.h"

namespace libera::helios {
namespace {

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

std::string makeHeliosUsbPortPath(libusb_device* device) {
    if (!device) {
        return "unknown";
    }

    std::array<std::uint8_t, 8> ports{};
    const int depth = libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
    if (depth > 0) {
        std::string path;
        for (int i = 0; i < depth; ++i) {
            if (!path.empty()) {
                path += "-";
            }
            path += std::to_string(static_cast<unsigned>(ports[i]));
        }
        if (!path.empty()) {
            return path;
        }
    }

    return "bus" + std::to_string(static_cast<unsigned>(libusb_get_bus_number(device))) +
           "-dev" + std::to_string(static_cast<unsigned>(libusb_get_device_address(device)));
}

bool announceHeliosSdkVersion(libusb_device_handle* handle) {
    if (handle == nullptr) {
        return false;
    }

    int actualLength = 0;
    const std::uint8_t request[2] = {0x07, HELIOS_SDK_VERSION};
    for (int i = 0; i < 2; ++i) {
        const int rc = libusb_interrupt_transfer(handle,
                                                 EP_INT_OUT,
                                                 const_cast<unsigned char*>(request),
                                                 2,
                                                 &actualLength,
                                                 32);
        if (rc == LIBUSB_SUCCESS && actualLength == 2) {
            return true;
        }
    }

    return false;
}

int queryHeliosUsbFirmwareVersion(libusb_device_handle* handle) {
    if (handle == nullptr) {
        return 0;
    }

    for (int i = 0; i < 2; ++i) {
        const std::uint8_t request[2] = {0x04, 0};
        int actualLength = 0;
        const int sendRc = libusb_interrupt_transfer(handle,
                                                     EP_INT_OUT,
                                                     const_cast<unsigned char*>(request),
                                                     2,
                                                     &actualLength,
                                                     32);
        if (sendRc != LIBUSB_SUCCESS || actualLength != 2) {
            continue;
        }

        for (int j = 0; j < 3; ++j) {
            std::array<std::uint8_t, 32> response{};
            const int receiveRc = libusb_interrupt_transfer(handle,
                                                            EP_INT_IN,
                                                            response.data(),
                                                            static_cast<int>(response.size()),
                                                            &actualLength,
                                                            32);
            if (receiveRc != LIBUSB_SUCCESS || actualLength < 5 || response[0] != 0x84) {
                continue;
            }

            return (response[1] << 0) |
                   (response[2] << 8) |
                   (response[3] << 16) |
                   (response[4] << 24);
        }
    }

    return 0;
}

struct HeliosUsbOpenResult {
    libusb_device_handle* handle = nullptr;
    int firmwareVersion = 0;
};

HeliosUsbOpenResult openHeliosUsbConnection(libusb_context* usbContext,
                                            const std::string& controllerPortPath) {
    HeliosUsbOpenResult result;
    if (usbContext == nullptr || controllerPortPath.empty()) {
        return result;
    }

    // Direct USB connect intentionally enumerates raw libusb devices and claims
    // only the one matching the persisted port path.
    libusb_device** deviceList = nullptr;
    const ssize_t count = libusb_get_device_list(usbContext, &deviceList);
    if (count < 0 || !deviceList) {
        return result;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = deviceList[i];
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(device, &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != HELIOS_VID || descriptor.idProduct != HELIOS_PID) {
            continue;
        }
        if (makeHeliosUsbPortPath(device) != controllerPortPath) {
            continue;
        }

        libusb_device_handle* handle = nullptr;
        const int openRc = libusb_open(device, &handle);
        if (openRc != LIBUSB_SUCCESS || handle == nullptr) {
            continue;
        }

        const int claimRc = libusb_claim_interface(handle, 0);
        if (claimRc != LIBUSB_SUCCESS) {
            libusb_close(handle);
            continue;
        }

        const int altRc = libusb_set_interface_alt_setting(handle, 0, 1);
        if (altRc != LIBUSB_SUCCESS) {
            libusb_release_interface(handle, 0);
            libusb_close(handle);
            continue;
        }

        // Match the vendor SDK startup sequence before streaming begins. The
        // short delay and interrupt flush discard stale packets from an earlier
        // owner/session, which matters both on first connect and after a USB
        // reconnect.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::array<std::uint8_t, 32> flushBuffer{};
        int actualLength = 0;
        while (libusb_interrupt_transfer(handle,
                                         EP_INT_IN,
                                         flushBuffer.data(),
                                         static_cast<int>(flushBuffer.size()),
                                         &actualLength,
                                         5) == LIBUSB_SUCCESS) {
        }

        result.firmwareVersion = queryHeliosUsbFirmwareVersion(handle);
        (void)announceHeliosSdkVersion(handle);
        result.handle = handle;
        break;
    }

    libusb_free_device_list(deviceList, 1);
    return result;
}

} // namespace

namespace error_types = libera::core::error_types;

struct HeliosController::DirectUsbConnection {
    DirectUsbConnection(libusb_device_handle* deviceHandle, int fw)
        : handle(deviceHandle), firmwareVersion(fw) {
        // Keep one reusable transfer buffer per DAC instance. The Helios USB
        // path is hot and allocation churn here is unnecessary.
        bulkTransferBuffer.reserve((HELIOS_MAX_POINTS * 7) + 5);
    }

    ~DirectUsbConnection() {
        close();
    }

    bool isClosed() const {
        return closed.load(std::memory_order_relaxed);
    }

    void prepareForShutdown() {
        abandonHandleOnClose.store(true, std::memory_order_relaxed);
    }

    void close() {
        // Close is serialized with I/O so we do not race a write/status poll
        // against releasing the USB interface underneath it.
        const bool wasClosed = closed.exchange(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(ioMutex);
        closeHandleLocked(!wasClosed);
    }

    bool tryReconnect(libusb_context* context, const std::string& controllerPortPath) {
        if (context == nullptr || controllerPortPath.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(ioMutex);
        if (!isClosed()) {
            return true;
        }

        // A fatal libusb error leaves the old handle unusable. Close that
        // handle first, then run the exact same open/claim/startup sequence as
        // the initial connection path.
        closeHandleLocked(false);
        const HeliosUsbOpenResult reopened = openHeliosUsbConnection(context, controllerPortPath);
        if (reopened.handle == nullptr) {
            return false;
        }

        handle = reopened.handle;
        firmwareVersion.store(reopened.firmwareVersion, std::memory_order_relaxed);
        shutterOpen = false;
        closed.store(false, std::memory_order_relaxed);
        return true;
    }

    void requestReconnect() {
        closed.store(true, std::memory_order_relaxed);
    }

    int getStatus() {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!handle || isClosed()) {
            return HELIOS_ERROR_DEVICE_CLOSED;
        }

        // This mirrors the vendor SDK's simple USB-ready poll:
        // send status request 0x03, then read one interrupt response packet.
        std::uint8_t request[2] = {0x03, 0};
        const int sendRc = sendControlLocked(request, 2, 16);
        if (sendRc != HELIOS_SUCCESS) {
            // Propagate the actual error (including HELIOS_ERROR_LIBUSB_BASE +
            // LIBUSB_ERROR_TIMEOUT = -5007) so the run loop can correctly
            // classify a send-phase timeout as usb::timeout rather than
            // usb::statusError.
            return sendRc;
        }

        std::array<std::uint8_t, 32> response{};
        int actualLength = 0;
        const int rc = libusb_interrupt_transfer(handle,
                                                 EP_INT_IN,
                                                 response.data(),
                                                 static_cast<int>(response.size()),
                                                 &actualLength,
                                                 16);
        if (rc != LIBUSB_SUCCESS) {
            markClosedOnDisconnectLocked(rc);
            return HELIOS_ERROR_LIBUSB_BASE + rc;
        }
        if (actualLength < 2 || response[0] != 0x83) {
            return HELIOS_ERROR_DEVICE_RESULT;
        }

        return response[1] == 0 ? 0 : 1;
    }

    int writeFrameExtended(unsigned int pps,
                           unsigned int flags,
                           const HeliosPointExt* points,
                           unsigned int numOfPoints) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!handle || isClosed()) {
            return HELIOS_ERROR_DEVICE_CLOSED;
        }
        if (points == nullptr || numOfPoints == 0) {
            return HELIOS_ERROR_NULL_POINTS;
        }

        // Keep behavior intentionally aligned with the vendor SDK rather than
        // inventing a new Helios transport policy here. The aim of this path is
        // ownership granularity, not changing frame semantics.
        std::vector<HeliosPointExt> duplicatedPoints;
        const HeliosPointExt* pointsPtr = points;

        if (pps < HELIOS_MIN_PPS) {
            if (pps == 0) {
                return HELIOS_ERROR_PPS_TOO_LOW;
            }

            unsigned int lowRateFactor = HELIOS_MIN_PPS / pps + 1;
            if (numOfPoints * lowRateFactor > HELIOS_MAX_POINTS) {
                return HELIOS_ERROR_PPS_TOO_LOW;
            }

            duplicatedPoints.resize(static_cast<std::size_t>(numOfPoints) * lowRateFactor);
            unsigned int writeIndex = 0;
            for (unsigned int i = 0; i < numOfPoints; ++i) {
                for (unsigned int j = 0; j < lowRateFactor; ++j) {
                    duplicatedPoints[writeIndex++] = points[i];
                }
            }

            pointsPtr = duplicatedPoints.data();
            numOfPoints *= lowRateFactor;
            pps *= lowRateFactor;
        }

        unsigned int samplingFactor = 1;
        if (pps > HELIOS_MAX_PPS || numOfPoints > HELIOS_MAX_POINTS) {
            samplingFactor = pps / HELIOS_MAX_PPS + 1;
            samplingFactor =
                std::max<unsigned int>(samplingFactor, numOfPoints / HELIOS_MAX_POINTS + 1);

            pps = pps / samplingFactor;
            numOfPoints = numOfPoints / samplingFactor;

            if (pps < HELIOS_MIN_PPS) {
                return HELIOS_ERROR_TOO_MANY_POINTS;
            }
        }

        unsigned int ppsActual = pps;
        unsigned int numOfPointsActual = numOfPoints;
        if ((((int)numOfPoints - 45) % 64) == 0) {
            // Preserve the Helios SDK workaround for the MCU transfer-size edge
            // case. Matching that quirk avoids introducing subtle USB-only
            // regressions while replacing the ownership model.
            numOfPointsActual--;
            ppsActual = static_cast<unsigned int>(
                (pps * static_cast<double>(numOfPointsActual) / static_cast<double>(numOfPoints)) + 0.5);
        }

        bulkTransferBuffer.clear();
        bulkTransferBuffer.resize(static_cast<std::size_t>(numOfPointsActual) * 7 + 5);
        unsigned int bufPos = 0;
        const unsigned int loopLength = numOfPointsActual * samplingFactor;
        for (unsigned int i = 0; i < loopLength; i += samplingFactor) {
            const auto& point = pointsPtr[i];
            const std::uint16_t x = point.x >> 4;
            const std::uint16_t y = point.y >> 4;
            bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(x >> 4);
            bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(((x & 0x0F) << 4) | (y >> 8));
            bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(y & 0xFF);
            bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(point.r >> 8);
            bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(point.g >> 8);
            bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(point.b >> 8);
            bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(point.i >> 8);
        }
        bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(ppsActual & 0xFF);
        bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(ppsActual >> 8);
        bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(numOfPointsActual & 0xFF);
        bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(numOfPointsActual >> 8);
        bulkTransferBuffer[bufPos++] = static_cast<std::uint8_t>(flags);

        if (!shutterOpen) {
            // Match the SDK behavior of auto-opening the shutter on first write.
            const int shutterRc = setShutterLocked(true);
            if (shutterRc != HELIOS_SUCCESS) {
                return shutterRc;
            }
        }

        int actualLength = 0;
        const int timeoutMs = 8 + static_cast<int>(bufPos >> 5);
        const int rc = libusb_bulk_transfer(handle,
                                            EP_BULK_OUT,
                                            bulkTransferBuffer.data(),
                                            static_cast<int>(bufPos),
                                            &actualLength,
                                            timeoutMs);
        if (rc != LIBUSB_SUCCESS) {
            markClosedOnDisconnectLocked(rc);
            return HELIOS_ERROR_LIBUSB_BASE + rc;
        }
        if (actualLength != static_cast<int>(bufPos)) {
            return HELIOS_ERROR_DEVICE_RESULT;
        }

        return HELIOS_SUCCESS;
    }

    int getFirmwareVersion() const {
        return firmwareVersion.load(std::memory_order_relaxed);
    }

    int getName(char* out) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!handle || isClosed()) {
            return HELIOS_ERROR_DEVICE_CLOSED;
        }
        std::uint8_t request[2] = {0x05, 0};
        const int sendRc = sendControlLocked(request, 2, 32);
        if (sendRc != HELIOS_SUCCESS) {
            return sendRc;
        }
        std::array<std::uint8_t, 32> response{};
        int actualLength = 0;
        const int rc = libusb_interrupt_transfer(handle, EP_INT_IN, response.data(),
                                                 static_cast<int>(response.size()), &actualLength, 32);
        if (rc != LIBUSB_SUCCESS) {
            markClosedOnDisconnectLocked(rc);
            return HELIOS_ERROR_LIBUSB_BASE + rc;
        }
        if (actualLength < 1 || response[0] != 0x85) {
            return HELIOS_ERROR_DEVICE_RESULT;
        }
        std::memcpy(out, response.data() + 1, 31);
        out[31] = '\0';
        return HELIOS_SUCCESS;
    }

    int setName(const char* name) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!handle || isClosed()) {
            return HELIOS_ERROR_DEVICE_CLOSED;
        }
        std::uint8_t txBuffer[32] = {0x06};
        std::strncpy(reinterpret_cast<char*>(txBuffer + 1), name, 30);
        txBuffer[31] = '\0';
        return sendControlLocked(txBuffer, 32, 32);
    }

private:
    void closeHandleLocked(bool sendStop) {
        if (!handle) {
            shutterOpen = false;
            return;
        }

        if (abandonHandleOnClose.load(std::memory_order_relaxed)) {
            // Teardown policy:
            // on macOS we have seen libusb_close() crash during app shutdown
            // from the Helios direct USB path. At this point the streaming
            // thread has already been stopped by the manager, and the process is
            // on its way out, so abandoning the raw handle is safer than trying
            // to make one last libusb call into an unstable teardown state.
            //
            // This is intentionally shutdown-only. Normal runtime close paths
            // still release the interface and close the handle properly.
            handle = nullptr;
            shutterOpen = false;
            return;
        }

        if (sendStop) {
            // Best-effort stop before releasing the USB interface. Use a raw
            // transfer here because close() has already marked the connection
            // closed so normal control helpers intentionally reject I/O.
            std::uint8_t stopRequest[2] = {0x01, 0};
            int actualLength = 0;
            (void)libusb_interrupt_transfer(handle,
                                            EP_INT_OUT,
                                            stopRequest,
                                            2,
                                            &actualLength,
                                            16);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        libusb_release_interface(handle, 0);
        libusb_close(handle);
        handle = nullptr;
        shutterOpen = false;
    }

    int sendControlLocked(std::uint8_t* buffer, unsigned int length, unsigned int timeoutMs) {
        if (buffer == nullptr) {
            return HELIOS_ERROR_DEVICE_NULL_BUFFER;
        }
        if (length > 32) {
            return HELIOS_ERROR_DEVICE_SIGNAL_TOO_LONG;
        }
        if (!handle || isClosed()) {
            return HELIOS_ERROR_DEVICE_CLOSED;
        }

        // All low-level USB control messages funnel through one helper so close,
        // status, shutter, and startup signaling share the same locking and
        // disconnect handling behavior.
        int actualLength = 0;
        const int rc = libusb_interrupt_transfer(handle,
                                                 EP_INT_OUT,
                                                 buffer,
                                                 static_cast<int>(length),
                                                 &actualLength,
                                                 timeoutMs);
        if (rc != LIBUSB_SUCCESS) {
            markClosedOnDisconnectLocked(rc);
            return HELIOS_ERROR_LIBUSB_BASE + rc;
        }
        if (actualLength != static_cast<int>(length)) {
            return HELIOS_ERROR_DEVICE_RESULT;
        }

        return HELIOS_SUCCESS;
    }

    int setShutterLocked(bool level) {
        std::uint8_t request[2] = {0x02, static_cast<std::uint8_t>(level ? 1 : 0)};
        const int rc = sendControlLocked(request, 2, 16);
        if (rc == HELIOS_SUCCESS) {
            shutterOpen = level;
        }
        return rc;
    }

    void markClosedOnDisconnectLocked(int libusbRc) {
        // Once macOS/libusb reports the device disappearing or a fatal I/O
        // breakage, stop pretending this handle is healthy. The run loop will
        // surface that as a connection loss and reopen the same port path.
        if (libusbRc == LIBUSB_ERROR_NO_DEVICE || libusbRc == LIBUSB_ERROR_IO) {
            closed.store(true, std::memory_order_relaxed);
        }
    }

    libusb_device_handle* handle = nullptr;
    std::atomic<bool> closed{false};
    std::atomic<bool> abandonHandleOnClose{false};
    bool shutterOpen = false;
    std::atomic<int> firmwareVersion{0};
    std::mutex ioMutex;
    std::vector<std::uint8_t> bulkTransferBuffer;
};

std::shared_ptr<HeliosController> HeliosController::connectUsb(
    std::shared_ptr<libusb_context> usbContext,
    std::string controllerPortPath) {
    if (!usbContext || controllerPortPath.empty()) {
        return {};
    }

    const HeliosUsbOpenResult opened =
        openHeliosUsbConnection(usbContext.get(), controllerPortPath);
    if (opened.handle == nullptr) {
        return {};
    }

    auto directConnection =
        std::make_unique<DirectUsbConnection>(opened.handle, opened.firmwareVersion);

    return std::shared_ptr<HeliosController>(
        new HeliosController(std::move(usbContext),
                             std::move(controllerPortPath),
                             std::move(directConnection)));
}

HeliosController::HeliosController(std::shared_ptr<libusb_context> usbContextValue,
                                   std::string controllerPortPath,
                                   std::unique_ptr<DirectUsbConnection> directConnection)
    : usbContext(std::move(usbContextValue))
    , usbPortPath(std::move(controllerPortPath))
    , usbConnection(std::move(directConnection)) {
    // USB path shares the same scheduling/buffering behavior as the legacy SDK
    // path so the higher-level controller contract stays unchanged.
    const auto defaultFramePoints = detail::defaultFramePointCount(getPointRate());
    targetFramePoints.store(defaultFramePoints, std::memory_order_relaxed);
    frameBuffer.reserve(defaultFramePoints);
    setEstimatedBufferCapacity(static_cast<int>(defaultFramePoints));
    updateEstimatedBufferSnapshotNow(0, getPointRate());
    statusWarmupDeadline =
        std::chrono::steady_clock::now() + detail::STATUS_ERROR_WARMUP_GRACE;
}

HeliosController::~HeliosController() {
    stopThread();
    close();
}

void HeliosController::prepareForShutdown() {
    if (usbConnection) {
        usbConnection->prepareForShutdown();
    }
}

void HeliosController::close() {
    setConnectionState(false);
    clearFrameTransportSubmissionEstimate();
    lastSubmittedFramePoints.store(0, std::memory_order_relaxed);
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
    if (usbConnection) {
        // For direct USB, closing means releasing exactly one DAC's interface.
        usbConnection->close();
    }
}

bool HeliosController::isConnected() const {
    if (usbConnection) {
        return !usbConnection->isClosed();
    }
    return false;
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
    const auto clamped = std::clamp<std::size_t>(points,
                                                 detail::MIN_FRAME_POINTS,
                                                 HELIOS_MAX_POINTS);
    framePointCountExplicitlySet.store(true, std::memory_order_relaxed);
    targetFramePoints.store(clamped, std::memory_order_relaxed);
    frameBuffer.reserve(clamped);
    setEstimatedBufferCapacity(static_cast<int>(clamped));
}

std::size_t HeliosController::framePointCount() const {
    return targetFramePoints.load(std::memory_order_relaxed);
}

int HeliosController::getFirmwareVersion() const {
    if (usbConnection) {
        return usbConnection->getFirmwareVersion();
    }
    return 0;
}

std::string HeliosController::getDacName() const {
    char buf[32] = {0};
    if (usbConnection) {
        usbConnection->getName(buf);
    }
    return std::string(buf);
}

bool HeliosController::setDacName(const std::string& name) {
    std::string truncated = name.substr(0, 30);
    char buf[32] = {0};
    std::strncpy(buf, truncated.c_str(), 30);
    if (usbConnection) {
        return usbConnection->setName(buf) == HELIOS_SUCCESS;
    }
    return false;
}

void HeliosController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();
    bool wasConnected = false;

    while (running) {
        const bool backendConnected = usbConnection && !usbConnection->isClosed();
        if (!backendConnected) {
            if (wasConnected) {
                recordConnectionError(error_types::usb::connectionLost);
            }
            setConnectionState(false);
            clearFrameTransportSubmissionEstimate();
            lastSubmittedFramePoints.store(0, std::memory_order_relaxed);
            estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
            updateEstimatedBufferSnapshotNow(0, getPointRate());
            wasConnected = false;

            const auto now = std::chrono::steady_clock::now();
            if (usbConnection && now >= nextReconnectAttempt) {
                ++reconnectAttemptCount;
                if (usbConnection->tryReconnect(usbContext.get(), usbPortPath)) {
                    logInfo("[HeliosController] USB reconnected",
                            "path", usbPortPath,
                            "attempts", reconnectAttemptCount);
                    reconnectAttemptCount = 0;
                    consecutiveStatusErrors = 0;
                    consecutiveStatusTimeouts = 0;
                    consecutiveWriteErrors = 0;
                    consecutiveWriteTimeouts = 0;
                    nextReconnectAttempt = std::chrono::steady_clock::time_point{};
                    setConnectionState(true);
                    resetStartupBlank();
                    statusWarmupDeadline =
                        std::chrono::steady_clock::now() + detail::STATUS_ERROR_WARMUP_GRACE;
                    wasConnected = true;
                    std::this_thread::sleep_for(5ms);
                    continue;
                }

                recordIntermittentError(error_types::usb::connectFailed);
                if (detail::shouldLogErrorBurst(reconnectAttemptCount)) {
                    logInfo("[HeliosController] waiting for USB reconnect",
                            "path", usbPortPath,
                            "attempt", reconnectAttemptCount);
                }
                nextReconnectAttempt = now + detail::USB_RECONNECT_INTERVAL;
            }

            std::this_thread::sleep_for(100ms);
            continue;
        }

        setConnectionState(true);
        if (!wasConnected) {
            resetStartupBlank();
            statusWarmupDeadline =
                std::chrono::steady_clock::now() + detail::STATUS_ERROR_WARMUP_GRACE;
        }
        wasConnected = true;

        const int status = usbConnection->getStatus();
        if (status < 0) {
            if (std::chrono::steady_clock::now() < statusWarmupDeadline) {
                consecutiveStatusErrors = 0;
                consecutiveStatusTimeouts = 0;
                std::this_thread::sleep_for(2ms);
                continue;
            }
            if (status == -5007) {
                recordIntermittentError(error_types::usb::timeout);
                ++consecutiveStatusTimeouts;
                consecutiveStatusErrors = 0;
                if (detail::shouldLogErrorBurst(consecutiveStatusTimeouts)) {
                    logError("[HeliosController] status timeout",
                             "path", usbPortPath,
                             "code", status,
                             "reason", describeHeliosError(status),
                             "consecutive", consecutiveStatusTimeouts);
                }
                if (consecutiveStatusTimeouts == detail::USB_RECONNECT_ERROR_THRESHOLD &&
                    usbConnection && !usbConnection->isClosed()) {
                    // A physically unplugged Helios can report as repeated
                    // USB timeouts on macOS rather than LIBUSB_ERROR_NO_DEVICE.
                    // Treat only a sustained timeout streak as a lost handle.
                    logError("[HeliosController] requesting USB reconnect after status timeouts",
                             "path", usbPortPath,
                             "consecutive", consecutiveStatusTimeouts);
                    usbConnection->requestReconnect();
                }
                std::this_thread::sleep_for(2ms);
                continue;
            }
            recordIntermittentError(error_types::usb::statusError);
            ++consecutiveStatusErrors;
            consecutiveStatusTimeouts = 0;
            if (detail::shouldLogErrorBurst(consecutiveStatusErrors)) {
                logError("[HeliosController] status error",
                         "path", usbPortPath,
                         "code", status,
                         "reason", describeHeliosError(status),
                         "consecutive", consecutiveStatusErrors);
            }
            if (consecutiveStatusErrors == detail::USB_RECONNECT_ERROR_THRESHOLD &&
                usbConnection && !usbConnection->isClosed()) {
                logError("[HeliosController] requesting USB reconnect after status errors",
                         "path", usbPortPath,
                         "consecutive", consecutiveStatusErrors);
                usbConnection->requestReconnect();
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        statusWarmupDeadline = std::chrono::steady_clock::time_point{};
        consecutiveStatusErrors = 0;
        consecutiveStatusTimeouts = 0;

        if (status == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        const std::size_t framePoints = targetFramePoints.load(std::memory_order_relaxed);
        const unsigned int pps = getPointRate();

        // Use the shared projection from LaserController so the FrameScheduler's
        // due-time gate fires when the just-submitted frame will *actually*
        // start playing — not when the bulk USB transfer finishes. Without this
        // a Liberation-side latency target of e.g. 150ms causes the scheduler
        // to loop the current frame for ~one frame period whenever the upstream
        // submission rate dips below the DAC's playback rate, because the next
        // queued frame still looks "not yet due" relative to a now+writeLead
        // estimate.
        const auto writeLead = detail::requestRenderLead(
            std::chrono::microseconds(
                estimatedWriteLeadMicros.load(std::memory_order_relaxed)));
        const auto now = std::chrono::steady_clock::now();
        const auto estimatedFirstRenderTime =
            projectedNextWriteRenderTime(now, writeLead);
        const auto pointIndex = currentPointIndex.load(std::memory_order_relaxed);

        core::Frame nativeFrame;

        // Direct Helios USB is a frame-ingester transport. It always wants one
        // complete submission for each free device slot, regardless of whether
        // the active source is queued frames or a live point callback.
        FrameFillRequest req;
        req.maximumPointsRequired = HELIOS_MAX_POINTS;
        req.preferredPointCount = framePoints;
        req.blankFramePointCount = framePoints;
        req.estimatedFirstPointRenderTime = estimatedFirstRenderTime;
        req.currentPointIndex = pointIndex;
        // Helios drains the queue in submitted order — latency is enforced as
        // queue depth via isReadyForNewFrame(), not as per-frame `time` gates.
        // Without this, drift between Liberation's submission cadence and the
        // DAC's playback period causes the scheduler to loop the current
        // frame whenever queue[1] is "not yet due" relative to the projected
        // render time, which the user reads as "frames replaying multiple
        // times" / reduced effective frame rate.
        req.advanceWhenAvailable = true;

        if (!requestFrame(req, nativeFrame)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (nativeFrame.points.empty()) {
            continue;
        }

        detail::encodeFramePoints(nativeFrame.points, frameBuffer);

        const auto sendStart = std::chrono::steady_clock::now();
        const int result = usbConnection->writeFrameExtended(pps,
                                                             detail::HELIOS_FLAGS,
                                                             frameBuffer.data(),
                                                             static_cast<unsigned int>(frameBuffer.size()));
        const auto sendDone = std::chrono::steady_clock::now();

        if (result < 0) {
            if (result == -5007) {
                recordIntermittentError(error_types::usb::timeout);
                ++consecutiveWriteTimeouts;
                consecutiveWriteErrors = 0;
            } else {
                recordIntermittentError(error_types::usb::transferFailed);
                ++consecutiveWriteErrors;
                consecutiveWriteTimeouts = 0;
            }
            const auto consecutiveWriteFailures =
                result == -5007 ? consecutiveWriteTimeouts : consecutiveWriteErrors;
            if (detail::shouldLogErrorBurst(consecutiveWriteFailures)) {
                logError("[HeliosController] WriteFrameExtended failed",
                         "path", usbPortPath,
                         "code", result,
                         "reason", describeHeliosError(result),
                         "consecutive", consecutiveWriteFailures,
                         "point_count", frameBuffer.size(),
                         "pps", pps);
            }
            if (consecutiveWriteFailures == detail::USB_RECONNECT_ERROR_THRESHOLD &&
                usbConnection && !usbConnection->isClosed()) {
                logError(result == -5007
                             ? "[HeliosController] requesting USB reconnect after write timeouts"
                             : "[HeliosController] requesting USB reconnect after write errors",
                         "path", usbPortPath,
                         "consecutive", consecutiveWriteFailures);
                usbConnection->requestReconnect();
            }
        } else {
            consecutiveWriteErrors = 0;
            consecutiveWriteTimeouts = 0;
            recordLatencySample(sendDone - sendStart);
            const auto measuredWriteLeadMicros =
                std::chrono::duration_cast<std::chrono::microseconds>(sendDone - sendStart).count();
            const auto previousWriteLeadMicros =
                estimatedWriteLeadMicros.load(std::memory_order_relaxed);
            estimatedWriteLeadMicros.store(
                detail::smoothWriteLeadMicros(previousWriteLeadMicros, measuredWriteLeadMicros),
                std::memory_order_relaxed);
            const auto previousFramePoints =
                lastSubmittedFramePoints.load(std::memory_order_relaxed);
            noteFrameTransportSubmissionBounded(
                frameBuffer.size(),
                estimatedFirstRenderTime,
                pps,
                previousFramePoints);
            lastSubmittedFramePoints.store(frameBuffer.size(), std::memory_order_relaxed);
            currentPointIndex.fetch_add(frameBuffer.size(), std::memory_order_relaxed);
        }
    }
}

} // namespace libera::helios
