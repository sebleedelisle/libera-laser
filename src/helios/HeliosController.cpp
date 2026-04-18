#include "libera/helios/HeliosController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
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

constexpr std::size_t MIN_FRAME_POINTS = 20;
constexpr double TARGET_FRAME_DURATION_MS = 10.0;
constexpr auto STATUS_ERROR_WARMUP_GRACE = std::chrono::milliseconds(250);

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

bool shouldLogErrorBurst(std::size_t consecutiveCount) {
    // Always log first failure, then throttle to keep console readable.
    return consecutiveCount == 1 || (consecutiveCount % 25 == 0);
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
        if (closed.exchange(true, std::memory_order_relaxed)) {
            return;
        }

        // Close is serialized with I/O so we do not race a write/status poll
        // against releasing the USB interface underneath it.
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!handle) {
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

        // Best-effort stop before releasing the USB interface.
        std::uint8_t stopRequest[2] = {0x01, 0};
        (void)sendControlLocked(stopRequest, 2, 16);
        std::this_thread::sleep_for(std::chrono::microseconds(100));

        libusb_release_interface(handle, 0);
        libusb_close(handle);
        handle = nullptr;
        shutterOpen = false;
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
        bool freePoints = false;

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
            freePoints = true;
        }

        unsigned int samplingFactor = 1;
        if (pps > HELIOS_MAX_PPS || numOfPoints > HELIOS_MAX_POINTS) {
            samplingFactor = pps / HELIOS_MAX_PPS + 1;
            samplingFactor =
                std::max<unsigned int>(samplingFactor, numOfPoints / HELIOS_MAX_POINTS + 1);

            pps = pps / samplingFactor;
            numOfPoints = numOfPoints / samplingFactor;

            if (pps < HELIOS_MIN_PPS) {
                (void)freePoints;
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
        return firmwareVersion;
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
        // surface that as a connection loss and stop trying to stream.
        if (libusbRc == LIBUSB_ERROR_NO_DEVICE || libusbRc == LIBUSB_ERROR_IO) {
            closed.store(true, std::memory_order_relaxed);
        }
    }

    libusb_device_handle* handle = nullptr;
    std::atomic<bool> closed{false};
    std::atomic<bool> abandonHandleOnClose{false};
    bool shutterOpen = false;
    int firmwareVersion = 0;
    std::mutex ioMutex;
    std::vector<std::uint8_t> bulkTransferBuffer;
};

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

std::shared_ptr<HeliosController> HeliosController::connectUsb(
    std::shared_ptr<libusb_context> usbContext,
    std::string controllerPortPath) {
    if (!usbContext || controllerPortPath.empty()) {
        return {};
    }

    // Direct USB connect intentionally enumerates raw libusb devices and claims
    // only the one matching the persisted port path.
    //
    // This is the key fix for multi-Helios setups: connecting one DAC must not
    // implicitly claim every Helios USB DAC visible to the process.
    libusb_device** deviceList = nullptr;
    const ssize_t count = libusb_get_device_list(usbContext.get(), &deviceList);
    if (count < 0 || !deviceList) {
        return {};
    }

    std::unique_ptr<DirectUsbConnection> directConnection;
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

        // Mirror the startup sequence the SDK uses before it starts talking to
        // the DAC. The short delay and interrupt flush help avoid stale packets
        // from a previous owner/session contaminating the first control reads.
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

        // Match the vendor SDK constructor more closely here. The initial
        // firmware query plus SDK version announce gives the device one full
        // request/response roundtrip before the streaming thread starts polling
        // status, which reduces spurious first-poll USB errors.
        const int fw = queryHeliosUsbFirmwareVersion(handle);
        (void)announceHeliosSdkVersion(handle);
        directConnection = std::make_unique<DirectUsbConnection>(handle, fw);
        break;
    }

    libusb_free_device_list(deviceList, 1);
    if (!directConnection) {
        return {};
    }

    return std::shared_ptr<HeliosController>(
        new HeliosController(std::move(usbContext),
                             std::move(controllerPortPath),
                             std::move(directConnection)));
}

HeliosController::HeliosController(std::shared_ptr<HeliosDac> sdkInstance, unsigned int controllerIndex)
    : sdk(std::move(sdkInstance))
    , index(controllerIndex) {
    const auto defaultFramePoints = detail::defaultFramePointCount(getPointRate());
    targetFramePoints.store(defaultFramePoints, std::memory_order_relaxed);
    frameBuffer.reserve(defaultFramePoints);
    setEstimatedBufferCapacity(static_cast<int>(defaultFramePoints));
    updateEstimatedBufferSnapshotNow(0, getPointRate());
    statusWarmupDeadline = std::chrono::steady_clock::now() + STATUS_ERROR_WARMUP_GRACE;
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
    statusWarmupDeadline = std::chrono::steady_clock::now() + STATUS_ERROR_WARMUP_GRACE;
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
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
    if (sdk) {
        sdk->Stop(index.load(std::memory_order_relaxed));
    }
    if (usbConnection) {
        // For direct USB, closing means releasing exactly one DAC's interface.
        usbConnection->close();
    }
}

bool HeliosController::isConnected() const {
    if (sdk) {
        return sdk->GetIsClosed(index.load(std::memory_order_relaxed)) == 0;
    }
    if (usbConnection) {
        return !usbConnection->isClosed();
    }
    return false;
}

void HeliosController::updateControllerIndex(unsigned int controllerIndex) {
    if (!sdk) {
        return;
    }

    const unsigned int previousIndex =
        index.exchange(controllerIndex, std::memory_order_relaxed);
    if (previousIndex == controllerIndex) {
        return;
    }

    // IDN reconnect strategy:
    // when the manager remaps a stable unit ID to a different transient SDK
    // slot after a network rescan, flush short-term scheduling state so the
    // stream ramps back in cleanly on the new slot.
    consecutiveStatusErrors = 0;
    consecutiveWriteErrors = 0;
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
    clearFrameTransportSubmissionEstimate();
    statusWarmupDeadline = std::chrono::steady_clock::now() + STATUS_ERROR_WARMUP_GRACE;
    resetStartupBlank();
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

int HeliosController::getFirmwareVersion() const {
    if (sdk) {
        return sdk->GetFirmwareVersion(index.load(std::memory_order_relaxed));
    }
    if (usbConnection) {
        return usbConnection->getFirmwareVersion();
    }
    return 0;
}

std::string HeliosController::getDacName() const {
    char buf[32] = {0};
    if (sdk) {
        sdk->GetName(index.load(std::memory_order_relaxed), buf);
    } else if (usbConnection) {
        usbConnection->getName(buf);
    }
    return std::string(buf);
}

bool HeliosController::setDacName(const std::string& name) {
    std::string truncated = name.substr(0, 30);
    char buf[32] = {0};
    std::strncpy(buf, truncated.c_str(), 30);
    if (sdk) {
        return sdk->SetName(index.load(std::memory_order_relaxed), buf) == HELIOS_SUCCESS;
    }
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
        const unsigned int sdkIndex = index.load(std::memory_order_relaxed);

        // Support both backends behind one controller implementation:
        // - SDK/index path for legacy or non-USB cases
        // - direct libusb path for Helios USB
        const bool backendConnected = sdk ? (sdk->GetIsClosed(sdkIndex) == 0)
                                          : (usbConnection && !usbConnection->isClosed());
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
            statusWarmupDeadline = std::chrono::steady_clock::now() + STATUS_ERROR_WARMUP_GRACE;
        }
        wasConnected = true;

        const int status = sdk ? sdk->GetStatus(sdkIndex) : usbConnection->getStatus();
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
            if (shouldLogErrorBurst(consecutiveStatusErrors)) {
                logError("[HeliosController] status error",
                         sdk ? "index" : "path",
                         sdk ? std::to_string(sdkIndex) : usbPortPath,
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

        const auto writeLead = detail::requestRenderLead(
            std::chrono::microseconds(
                estimatedWriteLeadMicros.load(std::memory_order_relaxed)));
        const auto estimatedFirstRenderTime =
            std::chrono::steady_clock::now() + writeLead;
        const auto pointIndex = currentPointIndex.load(std::memory_order_relaxed);

        std::vector<core::LaserPoint>* sourcePoints = &pointsToSend;
        core::Frame nativeFrame;

        if (usbConnection) {
            // Direct Helios USB is a frame-ingester transport. It always wants
            // one complete submission for each free device slot, regardless of
            // whether the active content source is the frame queue or a live
            // point callback.
            FrameFillRequest req;
            req.maximumPointsRequired = HELIOS_MAX_POINTS;
            req.preferredPointCount = framePoints;
            req.blankFramePointCount = framePoints;
            req.estimatedFirstPointRenderTime = estimatedFirstRenderTime;
            req.currentPointIndex = pointIndex;

            if (!requestFrame(req, nativeFrame)) {
                std::this_thread::sleep_for(5ms);
                continue;
            }

            sourcePoints = &nativeFrame.points;
        } else {
            setEstimatedBufferCapacity(static_cast<int>(framePoints));
            updateEstimatedBufferSnapshotNow(0, pps);
            // Streaming backends and SDK-backed Helios paths still pull a
            // controller-sized point batch, which is then packed into one
            // transport frame for submission.
            core::PointFillRequest req;
            req.minimumPointsRequired = framePoints;
            req.maximumPointsRequired = framePoints;
            req.estimatedFirstPointRenderTime = estimatedFirstRenderTime;
            req.currentPointIndex = pointIndex;

            if (!requestPoints(req)) {
                std::this_thread::sleep_for(5ms);
                continue;
            }
        }

        if (sourcePoints->empty()) {
            continue;
        }

        frameBuffer.resize(sourcePoints->size());
        for (std::size_t i = 0; i < sourcePoints->size(); ++i) {
            const auto& p = (*sourcePoints)[i];
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
        const int result = sdk
            ? sdk->WriteFrameExtended(sdkIndex,
                                      pps,
                                      HELIOS_FLAGS,
                                      frameBuffer.data(),
                                      static_cast<unsigned int>(frameBuffer.size()))
            : usbConnection->writeFrameExtended(pps,
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
                         sdk ? "index" : "path",
                         sdk ? std::to_string(sdkIndex) : usbPortPath,
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
            if (usbConnection) {
                noteFrameTransportSubmission(
                    frameBuffer.size(),
                    estimatedFirstRenderTime,
                    pps);
            } else {
                setEstimatedBufferCapacity(static_cast<int>(frameBuffer.size()));
                updateEstimatedBufferSnapshot(
                    static_cast<int>(frameBuffer.size()),
                    sendDone,
                    pps);
            }
            currentPointIndex.fetch_add(frameBuffer.size(), std::memory_order_relaxed);
        }
    }
}

} // namespace libera::helios
