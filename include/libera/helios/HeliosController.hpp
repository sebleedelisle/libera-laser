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

namespace detail {
std::size_t defaultFramePointCount(std::uint32_t pointRate);
std::size_t minimumRequestPoints(std::size_t maxFramePoints);
std::chrono::steady_clock::duration requestRenderLead(std::chrono::microseconds previousWriteLead);
std::int64_t smoothWriteLeadMicros(std::int64_t previousMicros, std::int64_t currentMicros);
} // namespace detail

class HeliosController : public core::LaserController {
public:
    // Legacy constructor kept for the SDK-backed path. Today that path is still
    // relevant to the Helios-family network/IDN side, but Helios USB now uses
    // the direct libusb factory below so one process does not claim every DAC.
    explicit HeliosController(std::shared_ptr<HeliosDac> sdk, unsigned int controllerIndex);
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
    void updateControllerIndex(unsigned int controllerIndex);
    unsigned int controllerIndex() const { return index.load(std::memory_order_relaxed); }
    const std::string& controllerPortPath() const { return usbPortPath; }

    void setPointRate(std::uint32_t pointRateValue) override;

    void setFramePointCount(std::size_t points);
    std::size_t framePointCount() const;

protected:
    void run() override;

private:
    struct DirectUsbConnection;

    // Private constructor for the direct Helios USB path.
    HeliosController(std::shared_ptr<libusb_context> usbContext,
                     std::string controllerPortPath,
                     std::unique_ptr<DirectUsbConnection> directConnection);

    std::shared_ptr<HeliosDac> sdk;
    std::shared_ptr<libusb_context> usbContext;
    std::string usbPortPath;
    std::unique_ptr<DirectUsbConnection> usbConnection;
    std::atomic<unsigned int> index{0};
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
