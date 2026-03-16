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
#define _WINSOCKAPI_
#endif
#include "libusb.h"

namespace libera::lasercubeusb {

namespace error_types = libera::core::error_types;

class UsbControllerHandle {
public:
    explicit UsbControllerHandle(libusb_device* controller) {
        int rc = libusb_open(controller, &handle);
        if (rc != 0 || !handle) {
            throw std::runtime_error(std::string("Failed to open USB controller: ") + libusb_error_name(rc));
        }

        claimInterface(0);
        claimInterface(1);

        rc = libusb_set_interface_alt_setting(handle, 1, 1);
        if (rc != 0) {
            throw std::runtime_error(std::string("Failed to set alt setting: ") + libusb_error_name(rc));
        }
    }

    ~UsbControllerHandle() {
        releaseInterface(0);
        releaseInterface(1);
        if (handle) {
            libusb_close(handle);
            handle = nullptr;
        }
    }

    libusb_device_handle* get() const { return handle; }

private:
    void claimInterface(int iface) {
        const int rc = libusb_claim_interface(handle, iface);
        if (rc != 0) {
            throw std::runtime_error(std::string("Failed to claim interface ") + std::to_string(iface) + ": " +
                                     libusb_error_name(rc));
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
    }

    libusb_device_handle* handle = nullptr;
};

namespace {

constexpr std::uint8_t CONTROL_ENDPOINT_OUT = 1 | LIBUSB_ENDPOINT_OUT;
constexpr std::uint8_t CONTROL_ENDPOINT_IN = 1 | LIBUSB_ENDPOINT_IN;
constexpr std::uint8_t DATA_ENDPOINT_OUT = 3 | LIBUSB_ENDPOINT_OUT;
constexpr int MIN_PACKET_DATA_SIZE = 128;
constexpr int SAFETY_HEADROOM_POINTS = MIN_PACKET_DATA_SIZE;
constexpr int WAIT_LEAD_MILLIS = 30;

bool send_uint8(libusb_device_handle* handle, std::uint8_t command, std::uint8_t value) {
    int transferred = 0;
    std::uint8_t packet[64] = {};
    packet[0] = command;
    packet[1] = value;

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet, 2, &transferred, 0);
    if (rc != 0 || transferred != 2) {
        return false;
    }

    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, packet, 64, &transferred, 0);
    if (rc != 0 || transferred != 64 || packet[1] != 0) {
        return false;
    }

    return true;
}

bool read_uint32(libusb_device_handle* handle, std::uint8_t command, std::uint32_t* value) {
    int transferred = 0;
    std::uint8_t packet[64] = {};
    packet[0] = command;

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet, 1, &transferred, 0);
    if (rc != 0 || transferred != 1) {
        return false;
    }

    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, packet, 64, &transferred, 0);
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

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet, length, &transferred, 0);
    if (rc != 0 || transferred != length) {
        return false;
    }

    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, packet, 64, &transferred, 0);
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
    const std::uint32_t length = std::min<std::uint32_t>(requestLength, packet.size());
    std::memcpy(packet.data(), request, length);

    int rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_OUT, packet.data(), static_cast<int>(length), &transferred, 0);
    if (rc != 0 || transferred != static_cast<int>(length)) {
        return false;
    }

    std::array<std::uint8_t, 64> response{};
    rc = libusb_bulk_transfer(handle, CONTROL_ENDPOINT_IN, response.data(), static_cast<int>(response.size()), &transferred, 0);
    if (rc != 0 || transferred != static_cast<int>(response.size()) || response[1] != 0) {
        return false;
    }

    return true;
}

} // namespace

LaserCubeUsbController::LaserCubeUsbController(std::shared_ptr<libusb_context> context)
    : usbContext(std::move(context)) {}

LaserCubeUsbController::~LaserCubeUsbController() {
    stop();
    close();
}

libera::expected<void> LaserCubeUsbController::connect(const LaserCubeUsbControllerInfo& info) {
    if (!usbContext) {
        recordConnectionError(error_types::usb::connectFailed);
        return libera::unexpected(std::make_error_code(std::errc::not_connected));
    }

    libusb_device** deviceList = nullptr;
    const ssize_t count = libusb_get_device_list(usbContext.get(), &deviceList);
    if (count < 0 || !deviceList) {
        recordConnectionError(error_types::usb::connectFailed);
        return libera::unexpected(std::make_error_code(std::errc::io_error));
    }

    std::unique_ptr<UsbControllerHandle> matchedHandle;
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
        const int length = libusb_get_string_descriptor_ascii(handle, descriptor.iSerialNumber, buffer, sizeof(buffer));
        libusb_close(handle);
        if (length <= 0) {
            continue;
        }
        const std::string serial(reinterpret_cast<char*>(buffer), static_cast<std::size_t>(length));
        if (serial == info.serial()) {
            foundSerial = true;
            try {
                matchedHandle = std::make_unique<UsbControllerHandle>(controller);
            } catch (const std::exception& ex) {
                logError("[LaserCubeUsbController] USB open failed", ex.what());
            }
            break;
        }
    }

    libusb_free_device_list(deviceList, 1);

    if (!matchedHandle) {
        if (foundSerial) {
            recordConnectionError(error_types::usb::connectFailed);
            return libera::unexpected(std::make_error_code(std::errc::io_error));
        }
        recordConnectionError(error_types::usb::connectFailed);
        return libera::unexpected(std::make_error_code(std::errc::no_such_device));
    }
    usbHandle = std::move(matchedHandle);

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

    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x01, 0x01}.data(), 3);
    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x01, 0x00}.data(), 3);
    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x09, 0x01}.data(), 3);
    send_raw(usbHandle->get(), std::array<std::uint8_t, 3>{0xC0, 0x09, 0x00}.data(), 3);

    std::array<std::uint8_t, 64> runnerPacket{};
    runnerPacket[0] = 0xC0;
    runnerPacket[1] = 0x08;
    const std::uint16_t position = 0;
    const std::uint16_t countSamples = 7;
    std::memcpy(runnerPacket.data() + 2, &position, sizeof(position));
    std::memcpy(runnerPacket.data() + 4, &countSamples, sizeof(countSamples));
    std::memset(runnerPacket.data() + 6, 0xFF, countSamples * 8);
    send_raw(usbHandle->get(), runnerPacket.data(), runnerPacket.size());

    usbConnected.store(true, std::memory_order_relaxed);
    setConnectionState(true);

    const auto initialRate = maxPointRate.load(std::memory_order_relaxed) > 0
                                 ? std::min(getPointRate(), maxPointRate.load(std::memory_order_relaxed))
                                 : getPointRate();
    LaserControllerStreaming::setPointRate(initialRate);
    targetPps.store(initialRate, std::memory_order_relaxed);
    currentPps.store(0, std::memory_order_relaxed);
    lastDataSentTime = std::chrono::steady_clock::time_point{};
    lastDataSentBufferSize = 0;
    setEstimatedBufferCapacity(getTotalBufferCapacity());
    updateEstimatedBufferAnchorNow(0, initialRate);

    return {};
}

void LaserCubeUsbController::close() {
    usbConnected.store(false, std::memory_order_relaxed);
    setConnectionState(false);
    clearEstimatedBufferState();
    usbHandle.reset();
}

void LaserCubeUsbController::setPointRate(std::uint32_t pointRateValue) {
    auto maxRate = maxPointRate.load(std::memory_order_relaxed);
    if (maxRate > 0 && pointRateValue > maxRate) {
        pointRateValue = maxRate;
    }

    LaserControllerStreaming::setPointRate(pointRateValue);
    targetPps.store(pointRateValue, std::memory_order_relaxed);
}

void LaserCubeUsbController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();

    while (running.load()) {
        if (!usbConnected.load(std::memory_order_relaxed) || !usbHandle) {
            setConnectionState(false);
            std::this_thread::sleep_for(20ms);
            continue;
        }
        setConnectionState(true);

        const auto desired = targetPps.load(std::memory_order_relaxed);
        const auto active = currentPps.load(std::memory_order_relaxed);
        if (desired != active) {
            if (sendPointRate(desired)) {
                currentPps.store(desired, std::memory_order_relaxed);
            }
        }
        waitUntilReadyToSend();
        if (!sendPoints()) {
            std::this_thread::sleep_for(2ms);
        }
    }
}

bool LaserCubeUsbController::sendPointRate(std::uint32_t rate) {
    if (!usbHandle) {
        recordConnectionError(error_types::usb::connectionLost);
        return false;
    }
    const bool ok = send_uint32(usbHandle->get(), 0x82, rate);
    if (!ok) {
        recordIntermittentError(error_types::usb::statusError);
    }
    return ok;
}

void LaserCubeUsbController::waitUntilReadyToSend() {
    const auto rate = static_cast<int>(currentPps.load(std::memory_order_relaxed));
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
    const auto rate = currentPps.load(std::memory_order_relaxed);
    return calculateBufferFullnessFromAnchor(
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
        return false;
    }

    if (currentPps.load(std::memory_order_relaxed) == 0) {
        return true;
    }

    const int capacity = getTotalBufferCapacity();
    const int bufferFullness = estimateBufferFullness();
    const int targetBufferPoints = core::BufferEstimator::targetBufferPoints(
        currentPps.load(std::memory_order_relaxed),
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
    request.minimumPointsRequired = static_cast<std::size_t>(maxPointsToAdd);
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
            0);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            --retries;
        }
    } while (rc == LIBUSB_ERROR_TIMEOUT && retries > 0);

    if (rc != 0 || transferred != static_cast<int>(packetBuffer.size())) {
        logError("[LaserCubeUsbController] send failed", libusb_error_name(rc));
        usbConnected.store(false, std::memory_order_relaxed);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            recordIntermittentError(error_types::usb::timeout);
        } else {
            recordConnectionError(error_types::usb::transferFailed);
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const int estimatedAfterSend = clampBufferFullnessToCapacity(
        bufferFullness + static_cast<int>(pointsToSend.size()),
        capacity);
    lastDataSentTime = now;
    lastDataSentBufferSize = estimatedAfterSend;
    updateEstimatedBufferAnchor(
        estimatedAfterSend,
        now,
        currentPps.load(std::memory_order_relaxed));
    recordLatencySample(now - sendStartTime);

    return true;
}

} // namespace libera::lasercubeusb
