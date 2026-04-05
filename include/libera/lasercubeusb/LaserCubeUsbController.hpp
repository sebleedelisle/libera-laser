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
    bool sendPointRateToDevice(std::uint32_t rate) override;

private:
    bool sendPoints();
    void waitUntilReadyToSend();

    int estimateBufferFullness() const;
    int getTotalBufferCapacity() const;

    std::shared_ptr<libusb_context> usbContext;
    std::unique_ptr<LaserCubeUsbHandle> usbHandle;

    std::atomic<bool> usbConnected{false};
    std::atomic<std::uint32_t> maxPointRate{0};
    std::atomic<int> maxSamplesPerTransfer{0};

    std::chrono::steady_clock::time_point lastDataSentTime{};
    int lastDataSentBufferSize{0};

    core::ByteBuffer packetBuffer;
};

} // namespace libera::lasercubeusb
