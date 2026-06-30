#include "libera/lasercubeusb/LaserCubeUsbController.hpp"

#include "libera/core/BufferEstimator.hpp"
#include "libera/core/ByteBuffer.hpp"
#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/lasercubeusb/LaserCubeUsbConfig.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // Keep Windows headers from defining min/max macros that break std::min/std::max.
#endif
#define _WINSOCKAPI_
#endif
#include "libusb.h"

namespace libera::lasercubeusb {

namespace error_types = libera::core::error_types;

class LaserCubeUsbHandle {
public:
    explicit LaserCubeUsbHandle(libusb_device* controller) {
        int rc = libusb_open(controller, &handle);
        if (rc != 0 || !handle) {
            throw std::runtime_error(std::string("Failed to open USB controller: ") + libusb_error_name(rc));
        }

        try {
            claimInterface(0);
            claimInterface(1);

            rc = libusb_set_interface_alt_setting(handle, 1, 1);
            if (rc != 0) {
                throw std::runtime_error(std::string("Failed to set alt setting: ") + libusb_error_name(rc));
            }
        } catch (...) {
            close();
            throw;
        }
    }

    ~LaserCubeUsbHandle() {
        close();
    }

    libusb_device_handle* get() const { return handle; }

private:
    void claimInterface(int iface) {
        const int rc = libusb_claim_interface(handle, iface);
        if (rc != 0) {
            throw std::runtime_error(std::string("Failed to claim interface ") + std::to_string(iface) + ": " +
                                     libusb_error_name(rc));
        }
        if (iface == 0) {
            claimedInterface0 = true;
        } else if (iface == 1) {
            claimedInterface1 = true;
        }
    }

    void releaseInterface(int iface) {
        if (!handle) {
            return;
        }
        const int rc = libusb_release_interface(handle, iface);
        if (rc != 0) {
            logError("[LaserCubeUsbController] Failed to release interface", iface, libusb_error_name(rc));
        }
        if (iface == 0) {
            claimedInterface0 = false;
        } else if (iface == 1) {
            claimedInterface1 = false;
        }
    }

    void close() {
        if (claimedInterface1) {
            releaseInterface(1);
        }
        if (claimedInterface0) {
            releaseInterface(0);
        }
        if (handle) {
            libusb_close(handle);
            handle = nullptr;
        }
    }

    libusb_device_handle* handle = nullptr;
    bool claimedInterface0 = false;
    bool claimedInterface1 = false;
};

namespace {

constexpr std::uint8_t CONTROL_ENDPOINT_OUT = 1 | LIBUSB_ENDPOINT_OUT;
constexpr std::uint8_t CONTROL_ENDPOINT_IN = 1 | LIBUSB_ENDPOINT_IN;
constexpr std::uint8_t DATA_ENDPOINT_OUT = 3 | LIBUSB_ENDPOINT_OUT;
constexpr int MIN_PACKET_DATA_SIZE = 128;
constexpr int SAFETY_HEADROOM_POINTS = MIN_PACKET_DATA_SIZE;
constexpr int WAIT_LEAD_MILLIS = 30;
constexpr auto USB_RECONNECT_INTERVAL = std::chrono::seconds(2);
constexpr std::size_t USB_RECONNECT_ERROR_THRESHOLD = 10;

bool shouldLogErrorBurst(std::size_t consecutiveCount) {
    // Log the first failure and then throttle repeated USB noise while a cable
    // is unplugged or a device is still rebooting.
    return consecutiveCount == 1 || (consecutiveCount % 25 == 0);
}

libera::expected<std::unique_ptr<LaserCubeUsbHandle>>
openLaserCubeUsbHandle(libusb_context* context, const std::string& serial) {
    if (context == nullptr || serial.empty()) {
        return libera::unexpected(std::make_error_code(std::errc::not_connected));
    }

    libusb_device** deviceList = nullptr;
    const ssize_t count = libusb_get_device_list(context, &deviceList);
    if (count < 0 || !deviceList) {
        return libera::unexpected(std::make_error_code(std::errc::io_error));
    }

    std::unique_ptr<LaserCubeUsbHandle> matchedHandle;
    bool foundSerial = false;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* controller = deviceList[i];
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(controller, &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != LaserCubeUsbConfig::VENDOR_ID ||
            descriptor.idProduct != LaserCubeUsbConfig::PRODUCT_ID) {
            continue;
        }
        if (descriptor.iSerialNumber == 0) {
            continue;
        }

        libusb_device_handle* handle = nullptr;
        if (libusb_open(controller, &handle) != 0 || !handle) {
            continue;
        }
        unsigned char buffer[256] = {};
        const int length = libusb_get_string_descriptor_ascii(
            handle,
            descriptor.iSerialNumber,
            buffer,
            static_cast<int>(sizeof(buffer)));
        libusb_close(handle);
        if (length <= 0) {
            continue;
        }

        const std::string discoveredSerial(reinterpret_cast<char*>(buffer),
                                           static_cast<std::size_t>(length));
        if (discoveredSerial != serial) {
            continue;
        }

        foundSerial = true;
        try {
            matchedHandle = std::make_unique<LaserCubeUsbHandle>(controller);
        } catch (const std::exception& ex) {
            logError("[LaserCubeUsbController] USB open failed", ex.what());
        }
        break;
    }

    libusb_free_device_list(deviceList, 1);

    if (matchedHandle) {
        return std::move(matchedHandle);
    }
    if (foundSerial) {
        return libera::unexpected(std::make_error_code(std::errc::io_error));
    }
    return libera::unexpected(std::make_error_code(std::errc::no_such_device));
}

bool send_uint8(libusb_device_handle* handle, std::uint8_t command, std::uint8_t value) {
    int transferred = 0;
    std::uint8_t packet[64] = {};
    packet[0] = command;
    packet[1] = value;

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet, 2, &transferred, 1000);
    if (rc != 0 || transferred != 2) {
        return false;
    }

    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, packet, 64, &transferred, 1000);
    if (rc != 0 || transferred != 64 || packet[1] != 0) {
        return false;
    }

    return true;
}

bool read_uint32(libusb_device_handle* handle, std::uint8_t command, std::uint32_t* value) {
    int transferred = 0;
    std::uint8_t packet[64] = {};
    packet[0] = command;

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet, 1, &transferred, 1000);
    if (rc != 0 || transferred != 1) {
        return false;
    }

    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, packet, 64, &transferred, 1000);
    if (rc != 0 || transferred != 64 || packet[1] != 0) {
        return false;
    }

    std::memcpy(value, packet + 2, sizeof(std::uint32_t));
    return true;
}

bool send_uint32(libusb_device_handle* handle, std::uint8_t command, std::uint32_t value) {
    int transferred = 0;
    std::uint8_t packet[64] = {};
    packet[0] = command;
    std::memcpy(packet + 1, &value, sizeof(std::uint32_t));
    const int length = 1 + static_cast<int>(sizeof(std::uint32_t));

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet, length, &transferred, 1000);
    if (rc != 0 || transferred != length) {
        return false;
    }

    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, packet, 64, &transferred, 1000);
    if (rc != 0 || transferred != 64 || packet[1] != 0) {
        return false;
    }

    return true;
}

bool send_raw(libusb_device_handle* handle, const std::uint8_t* request, std::uint32_t requestLength) {
    if (!request || requestLength == 0) {
        return false;
    }

    int transferred = 0;
    std::array<std::uint8_t, 64> packet{};
    const std::uint32_t length = std::min(requestLength, static_cast<std::uint32_t>(packet.size()));
    std::memcpy(packet.data(), request, length);

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet.data(), static_cast<int>(length), &transferred, 1000);
    if (rc != 0 || transferred != static_cast<int>(length)) {
        return false;
    }

    std::array<std::uint8_t, 64> response{};
    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, response.data(), static_cast<int>(response.size()), &transferred, 1000);
    if (rc != 0 || transferred != static_cast<int>(response.size()) || response[1] != 0) {
        return false;
    }

    return true;
}

} // namespace

LaserCubeUsbController::LaserCubeUsbController(std::shared_ptr<libusb_context> context)
    : usbContext(std::move(context)) {}

LaserCubeUsbController::~LaserCubeUsbController() {
    stopThread();
    close();
}

libera::expected<void> LaserCubeUsbController::connect(const LaserCubeUsbControllerInfo& info) {
    markUsbDisconnected();
    usbSerial = info.serial();

    auto result = connectToSerial(usbSerial);
    if (!result) {
        recordConnectionError(error_types::usb::connectFailed);
    }
    return result;
}

libera::expected<void> LaserCubeUsbController::connectToSerial(const std::string& serial) {
    if (!usbContext) {
        return libera::unexpected(std::make_error_code(std::errc::not_connected));
    }

    auto opened = openLaserCubeUsbHandle(usbContext.get(), serial);
    if (!opened) {
        return libera::unexpected(opened.error());
    }
    usbHandle = std::move(opened.value());

    if (!send_uint8(usbHandle->get(), 0x80, 0x01)) {
        logError("[LaserCubeUsbController] Failed to enable output");
        recordIntermittentError(error_types::usb::statusError);
    }

    std::uint32_t maxRate = 0;
    if (read_uint32(usbHandle->get(), 0x84, &maxRate)) {
        maxPointRate.store(maxRate, std::memory_order_relaxed);
    } else {
        maxPointRate.store(0, std::memory_order_relaxed);
    }

    std::uint32_t bulkCount = 0;
    if (read_uint32(usbHandle->get(), 0x8E, &bulkCount) && bulkCount > 0) {
        maxSamplesPerTransfer.store(static_cast<int>(bulkCount), std::memory_order_relaxed);
    } else {
        maxSamplesPerTransfer.store(LaserCubeUsbConfig::BUFFER_CAPACITY, std::memory_order_relaxed);
    }

    // Runner mode initialization sequence (from LaserDock protocol):
    // Enable runner mode, then immediately disable it to reset firmware state.
    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x01, 0x01}.data(), 3); // runner_mode_enable(true)
    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x01, 0x00}.data(), 3); // runner_mode_enable(false)
    // Start runner, then stop it.
    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x09, 0x01}.data(), 3); // runner_mode_run(true)
    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x09, 0x00}.data(), 3); // runner_mode_run(false)

    // Load 7 all-white (0xFF) samples at position 0 into the runner buffer.
    // This leaves the firmware in a known state before streaming begins.
    std::array<std::uint8_t, 64> runnerPacket{};
    runnerPacket[0] = 0xC0;             // command
    runnerPacket[1] = 0x08;             // sub-command: runner_mode_load
    const std::uint16_t position = 0;
    const std::uint16_t countSamples = 7;
    std::memcpy(runnerPacket.data() + 2, &position, sizeof(position));
    std::memcpy(runnerPacket.data() + 4, &countSamples, sizeof(countSamples));
    std::memset(runnerPacket.data() + 6, 0xFF, countSamples * 8); // 7 samples × 8 bytes each
    send_raw(usbHandle->get(), runnerPacket.data(), static_cast<std::uint32_t>(runnerPacket.size()));

    usbConnected.store(true, std::memory_order_relaxed);
    setConnectionState(true);
    consecutiveControlErrors = 0;
    consecutiveTransferTimeouts = 0;
    consecutiveTransferErrors = 0;

    const auto initialRate = maxPointRate.load(std::memory_order_relaxed) > 0
                                 ? std::min(getPointRate(), maxPointRate.load(std::memory_order_relaxed))
                                 : getPointRate();
    LaserControllerStreaming::setPointRate(initialRate);
    // Device's internal rate is unknown after a fresh connection, so force
    // the next syncPointRate() tick to push the current value.
    pointRatePushNeeded = true;
    lastDataSentTime = std::chrono::steady_clock::time_point{};
    lastDataSentBufferSize = 0;
    setEstimatedBufferCapacity(getTotalBufferCapacity());
    updateEstimatedBufferSnapshotNow(0, initialRate);

    return {};
}

bool LaserCubeUsbController::tryReconnect() {
    if (!usbContext || usbSerial.empty()) {
        return false;
    }

    // Release any stale libusb handle before scanning. After a physical
    // disconnect the old handle is no longer useful, and holding it can make
    // the reappeared device look busy to our own process.
    markUsbDisconnected();
    return static_cast<bool>(connectToSerial(usbSerial));
}

void LaserCubeUsbController::markUsbDisconnected() {
    usbConnected.store(false, std::memory_order_relaxed);
    setConnectionState(false);
    clearEstimatedBufferState();
    lastDataSentTime = std::chrono::steady_clock::time_point{};
    lastDataSentBufferSize = 0;
    lastSentPointRate = 0;
    pointRatePushNeeded = true;
    usbHandle.reset();
}

void LaserCubeUsbController::close() {
    usbSerial.clear();
    reconnectAttemptCount = 0;
    consecutiveControlErrors = 0;
    consecutiveTransferTimeouts = 0;
    consecutiveTransferErrors = 0;
    nextReconnectAttempt = std::chrono::steady_clock::time_point{};
    markUsbDisconnected();
}

void LaserCubeUsbController::setPointRate(std::uint32_t pointRateValue) {
    auto maxRate = maxPointRate.load(std::memory_order_relaxed);
    if (maxRate > 0 && pointRateValue > maxRate) {
        pointRateValue = maxRate;
    }
    LaserControllerStreaming::setPointRate(pointRateValue);
}

void LaserCubeUsbController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();
    bool wasConnected = false;

    while (running.load()) {
        if (!usbConnected.load(std::memory_order_relaxed) || !usbHandle) {
            if (wasConnected) {
                recordConnectionError(error_types::usb::connectionLost);
            }
            setConnectionState(false);
            clearEstimatedBufferState();
            wasConnected = false;

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextReconnectAttempt) {
                ++reconnectAttemptCount;
                if (tryReconnect()) {
                    logInfo("[LaserCubeUsbController] USB reconnected",
                            "serial", usbSerial,
                            "attempts", reconnectAttemptCount);
                    reconnectAttemptCount = 0;
                    consecutiveControlErrors = 0;
                    consecutiveTransferTimeouts = 0;
                    consecutiveTransferErrors = 0;
                    nextReconnectAttempt = std::chrono::steady_clock::time_point{};
                    resetStartupBlank();
                    wasConnected = true;
                    std::this_thread::sleep_for(5ms);
                    continue;
                }

                recordIntermittentError(error_types::usb::connectFailed);
                if (shouldLogErrorBurst(reconnectAttemptCount)) {
                    logInfo("[LaserCubeUsbController] waiting for USB reconnect",
                            "serial", usbSerial,
                            "attempt", reconnectAttemptCount);
                }
                nextReconnectAttempt = now + USB_RECONNECT_INTERVAL;
            }

            std::this_thread::sleep_for(100ms);
            continue;
        }
        setConnectionState(true);
        wasConnected = true;
        syncPointRate();
        waitUntilReadyToSend();
        if (!sendPoints()) {
            std::this_thread::sleep_for(2ms);
        }
    }
}

void LaserCubeUsbController::syncPointRate() {
    const auto desired = getPointRate();
    // Short-circuit when nothing has changed and no resync is pending.
    if (!pointRatePushNeeded && desired == lastSentPointRate) {
        return;
    }
    if (!usbHandle) {
        recordConnectionError(error_types::usb::connectionLost);
        pointRatePushNeeded = true;
        markUsbDisconnected();
        return;
    }
    const bool ok = send_uint32(usbHandle->get(), 0x82, desired);
    if (ok) {
        lastSentPointRate = desired;
        pointRatePushNeeded = false;
        consecutiveControlErrors = 0;
    } else {
        recordIntermittentError(error_types::usb::statusError);
        ++consecutiveControlErrors;
        if (shouldLogErrorBurst(consecutiveControlErrors)) {
            logError("[LaserCubeUsbController] point-rate sync failed",
                     "serial", usbSerial,
                     "consecutive", consecutiveControlErrors);
        }
        if (consecutiveControlErrors == USB_RECONNECT_ERROR_THRESHOLD) {
            logError("[LaserCubeUsbController] requesting USB reconnect after control errors",
                     "serial", usbSerial,
                     "consecutive", consecutiveControlErrors);
            markUsbDisconnected();
        }
        // Leave the latch set so the next tick retries.
        pointRatePushNeeded = true;
    }
}

void LaserCubeUsbController::waitUntilReadyToSend() {
    const auto rate = static_cast<int>(lastSentPointRate);
    if (rate == 0) {
        return;
    }

    const int minPointsInBuffer = core::BufferEstimator::targetBufferPoints(
        static_cast<std::uint32_t>(rate),
        getTotalBufferCapacity(),
        targetLatency(),
        MIN_PACKET_DATA_SIZE,
        SAFETY_HEADROOM_POINTS);

    const int bufferFullness = estimateBufferFullness();
    const int pointsUntilNeedsRefill = std::max(MIN_PACKET_DATA_SIZE, bufferFullness - minPointsInBuffer);

    const double microsPerPoint = 1000000.0 / static_cast<double>(rate);
    int microsToWait = static_cast<int>(std::lround(pointsUntilNeedsRefill * microsPerPoint));
    microsToWait -= (WAIT_LEAD_MILLIS * 1000);
    if (microsToWait > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(microsToWait));
    }
}

int LaserCubeUsbController::estimateBufferFullness() const {
    const auto rate = lastSentPointRate;
    return calculateBufferFullnessFromSnapshot(
        lastDataSentBufferSize,
        lastDataSentTime,
        rate,
        0);
}

int LaserCubeUsbController::getTotalBufferCapacity() const {
    return LaserCubeUsbConfig::BUFFER_CAPACITY;
}

bool LaserCubeUsbController::sendPoints() {
    if (!usbHandle) {
        recordConnectionError(error_types::usb::connectionLost);
        markUsbDisconnected();
        return false;
    }

    const auto activeRate = lastSentPointRate;
    if (activeRate == 0) {
        return true;
    }

    const int capacity = getTotalBufferCapacity();
    const int bufferFullness = estimateBufferFullness();
    const int targetBufferPoints = core::BufferEstimator::targetBufferPoints(
        activeRate,
        capacity,
        targetLatency(),
        MIN_PACKET_DATA_SIZE,
        SAFETY_HEADROOM_POINTS);
    int maxPointsToAdd = targetBufferPoints - bufferFullness;
    if (maxPointsToAdd <= 0) {
        return true;
    }

    const int maxTransfer = maxSamplesPerTransfer.load(std::memory_order_relaxed);
    if (maxTransfer > 0 && maxPointsToAdd > maxTransfer) {
        maxPointsToAdd = maxTransfer;
    }

    core::PointFillRequest request{};
    // USB runner mode can accept larger bursts, but the worker loop only needs
    // one healthy packet's worth to make progress without stalling.
    request.minimumPointsRequired =
        static_cast<std::size_t>(std::min(maxPointsToAdd, MIN_PACKET_DATA_SIZE));
    request.maximumPointsRequired = static_cast<std::size_t>(maxPointsToAdd);

    if (!requestPoints(request)) {
        return false;
    }

    if (pointsToSend.empty()) {
        return true;
    }

    if (pointsToSend.size() > static_cast<std::size_t>(maxPointsToAdd)) {
        pointsToSend.resize(static_cast<std::size_t>(maxPointsToAdd));
    }

    packetBuffer.clear();
    for (const auto& pt : pointsToSend) {
        const std::uint16_t x = encodeUnsigned12FromSignedUnit(pt.x);
        const std::uint16_t y = encodeUnsigned12FromSignedUnit(pt.y);
        const std::uint8_t r = encodeUnsigned8FromUnit(pt.r);
        const std::uint8_t g = encodeUnsigned8FromUnit(pt.g);
        const std::uint8_t b = encodeUnsigned8FromUnit(pt.b);
        const std::uint16_t rg = static_cast<std::uint16_t>(r) | (static_cast<std::uint16_t>(g) << 8);
        const std::uint16_t bpacked = static_cast<std::uint16_t>(b);

        packetBuffer.appendUInt16(rg);
        packetBuffer.appendUInt16(bpacked);
        packetBuffer.appendUInt16(x);
        packetBuffer.appendUInt16(y);
    }

    int transferred = 0;
    int rc = 0;
    int retries = 3;
    const auto sendStartTime = std::chrono::steady_clock::now();
    do {
        rc = libusb_bulk_transfer(
            usbHandle->get(),
            DATA_ENDPOINT_OUT,
            reinterpret_cast<unsigned char*>(packetBuffer.data()),
            static_cast<int>(packetBuffer.size()),
            &transferred,
            100);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            --retries;
        }
    } while (rc == LIBUSB_ERROR_TIMEOUT && retries > 0);

    const int expectedTransferSize = static_cast<int>(packetBuffer.size());
    if (rc != 0 || transferred != expectedTransferSize) {
        const bool timeout = rc == LIBUSB_ERROR_TIMEOUT;
        const bool shortTransfer = rc == LIBUSB_SUCCESS && transferred != expectedTransferSize;
        if (timeout) {
            recordIntermittentError(error_types::usb::timeout);
            ++consecutiveTransferTimeouts;
            consecutiveTransferErrors = 0;
            if (shouldLogErrorBurst(consecutiveTransferTimeouts)) {
                logError("[LaserCubeUsbController] send timeout",
                         "serial", usbSerial,
                         "consecutive", consecutiveTransferTimeouts,
                         "transferred", transferred,
                         "expected", expectedTransferSize);
            }
            if (consecutiveTransferTimeouts == USB_RECONNECT_ERROR_THRESHOLD) {
                logError("[LaserCubeUsbController] requesting USB reconnect after send timeouts",
                         "serial", usbSerial,
                         "consecutive", consecutiveTransferTimeouts);
                markUsbDisconnected();
            }
        } else {
            ++consecutiveTransferErrors;
            consecutiveTransferTimeouts = 0;
            logError("[LaserCubeUsbController] send failed",
                     "serial", usbSerial,
                     "reason", shortTransfer ? "short_transfer" : libusb_error_name(rc),
                     "consecutive", consecutiveTransferErrors,
                     "transferred", transferred,
                     "expected", expectedTransferSize);
            recordConnectionError(error_types::usb::transferFailed);
            markUsbDisconnected();
        }
        return false;
    }

    consecutiveTransferTimeouts = 0;
    consecutiveTransferErrors = 0;

    const auto now = std::chrono::steady_clock::now();
    const int estimatedAfterSend = clampBufferFullnessToCapacity(
        bufferFullness + static_cast<int>(pointsToSend.size()),
        capacity);
    lastDataSentTime = now;
    lastDataSentBufferSize = estimatedAfterSend;
    updateEstimatedBufferSnapshot(
        estimatedAfterSend,
        now,
        lastSentPointRate);
    recordLatencySample(now - sendStartTime);

    return true;
}

} // namespace libera::lasercubeusb
