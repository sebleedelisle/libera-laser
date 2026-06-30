#pragma once

#include "libera/core/Expected.hpp"
#include "libera/core/ByteBuffer.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/lasercubeusb/LaserCubeUsbControllerInfo.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

struct libusb_context;

namespace libera::lasercubeusb {

class LaserCubeUsbHandle;

class LaserCubeUsbController : public core::LaserController {
public:
    explicit LaserCubeUsbController(std::shared_ptr<libusb_context> context);
    ~LaserCubeUsbController() override;

    libera::expected<void> connect(const LaserCubeUsbControllerInfo& info);
    void close();

    void setPointRate(std::uint32_t pointRate) override;

protected:
    void run() override;

private:
    libera::expected<void> connectToSerial(const std::string& serial);
    bool tryReconnect();
    void markUsbDisconnected();

    /// Push the desired point rate to the device if it differs from the
    /// last-sent value, or if a forced re-push is pending after reconnect.
    void syncPointRate();

    bool sendPoints();
    void waitUntilReadyToSend();

    int estimateBufferFullness() const;
    int getTotalBufferCapacity() const;

    std::shared_ptr<libusb_context> usbContext;
    std::unique_ptr<LaserCubeUsbHandle> usbHandle;
    std::string usbSerial;

    std::atomic<bool> usbConnected{false};
    std::atomic<std::uint32_t> maxPointRate{0};
    std::atomic<int> maxSamplesPerTransfer{0};

    std::chrono::steady_clock::time_point lastDataSentTime{};
    int lastDataSentBufferSize{0};

    // Tracks the rate we've successfully told the device about. Worker-thread
    // only, so not atomic. Starts at 0 so the first tick always pushes.
    std::uint32_t lastSentPointRate{0};
    // Latched true on (re)connect so the next syncPointRate() tick force-sends
    // the rate even if it matches lastSentPointRate (stale after reconnect).
    bool pointRatePushNeeded{true};
    std::size_t consecutiveControlErrors{0};
    std::size_t consecutiveTransferTimeouts{0};
    std::size_t consecutiveTransferErrors{0};
    std::size_t reconnectAttemptCount{0};
    std::chrono::steady_clock::time_point nextReconnectAttempt{};

    core::ByteBuffer packetBuffer;
};

} // namespace libera::lasercubeusb
