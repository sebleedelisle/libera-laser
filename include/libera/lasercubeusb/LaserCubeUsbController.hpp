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

class UsbControllerHandle;

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
    bool sendPointsToDac();
    bool sendPointRate(std::uint32_t rate);
    void waitUntilReadyToSend();

    int estimateBufferFullness() const;
    int getDacTotalPointBufferCapacity() const;

    std::shared_ptr<libusb_context> usbContext;
    std::unique_ptr<UsbControllerHandle> usbHandle;

    std::atomic<bool> usbConnected{false};
    std::atomic<std::uint32_t> currentPps{0};
    std::atomic<std::uint32_t> targetPps{30000};
    std::atomic<std::uint32_t> maxPointRate{0};
    std::atomic<int> maxSamplesPerTransfer{0};

    std::chrono::steady_clock::time_point lastDataSentTime{};
    int lastDataSentBufferSize{0};

    core::ByteBuffer packetBuffer;
};

} // namespace libera::lasercubeusb
