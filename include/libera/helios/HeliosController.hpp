#pragma once

#include "libera/core/LaserController.hpp"
#include "HeliosDac.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct libusb_context;

namespace libera::helios {

class HeliosController : public core::LaserController {
public:
    // Direct Helios USB connection path.
    //
    // Why this exists:
    // the vendor SDK opens all Helios USB DACs together. That made one assigned
    // Helios cause every other Helios to appear "(in use)" from another app.
    // This factory opens only the selected physical DAC identified by port path.
    static std::shared_ptr<HeliosController> connectUsb(std::shared_ptr<libusb_context> usbContext,
                                                        std::string controllerPortPath);
    ~HeliosController() override;

    // Prepare the direct USB path for app shutdown. This intentionally trades a
    // tiny process-lifetime leak for avoiding libusb_close() crashes observed
    // during teardown on macOS.
    void prepareForShutdown();
    void close();
    bool isConnected() const;
    const std::string& controllerPortPath() const { return usbPortPath; }

    void setPointRate(std::uint32_t pointRateValue) override;

    void setFramePointCount(std::size_t points);
    std::size_t framePointCount() const;

    /// Returns the firmware version stored on the DAC.
    /// For USB Helios: a simple integer (≤ 255). For IDN: AABBCC format (vAA.BB.CC).
    int getFirmwareVersion() const;

    /// Returns the persistent name stored on the DAC hardware.
    std::string getDacName() const;

    /// Writes a new persistent name to the DAC hardware (max 30 characters).
    /// Returns true on success. The name survives power cycles.
    bool setDacName(const std::string& name);

protected:
    void run() override;

private:
    struct DirectUsbConnection;

    // Private constructor for the direct Helios USB path.
    HeliosController(std::shared_ptr<libusb_context> usbContext,
                     std::string controllerPortPath,
                     std::unique_ptr<DirectUsbConnection> directConnection);

    std::shared_ptr<libusb_context> usbContext;
    std::string usbPortPath;
    std::unique_ptr<DirectUsbConnection> usbConnection;
    std::atomic<std::size_t> targetFramePoints{1000};
    std::atomic<bool> framePointCountExplicitlySet{false};
    std::atomic<std::uint64_t> currentPointIndex{0};
    std::atomic<std::int64_t> estimatedWriteLeadMicros{0};
    std::vector<HeliosPointExt> frameBuffer;

    // Simple counters used for log throttling and health diagnostics.
    std::size_t consecutiveStatusErrors = 0;
    std::size_t consecutiveWriteErrors = 0;
    std::chrono::steady_clock::time_point statusWarmupDeadline{};
};

} // namespace libera::helios
