#pragma once

#include "libera/core/LaserController.hpp"
#include "HeliosDac.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libera::idn {

class IdnController : public core::LaserController {
public:
    explicit IdnController(std::shared_ptr<HeliosDac> sdk, unsigned int controllerIndex);
    ~IdnController() override;

    void close();
    bool isConnected() const;
    void updateControllerIndex(unsigned int controllerIndex);
    unsigned int controllerIndex() const { return index.load(std::memory_order_relaxed); }

    void setPointRate(std::uint32_t pointRateValue) override;

    void setFramePointCount(std::size_t points);
    std::size_t framePointCount() const;

    /// Returns the firmware version reported by the SDK-backed IDN device.
    int getFirmwareVersion() const;

    /// Returns the current DAC name as exposed by the Helios SDK.
    std::string getDacName() const;

    /// Writes a new DAC name through the Helios SDK (max 30 characters).
    bool setDacName(const std::string& name);

protected:
    void run() override;

private:
    std::shared_ptr<HeliosDac> sdk;
    std::atomic<unsigned int> index{0};
    std::atomic<std::size_t> targetFramePoints{1000};
    std::atomic<bool> framePointCountExplicitlySet{false};
    std::atomic<std::uint64_t> currentPointIndex{0};
    std::atomic<std::int64_t> estimatedWriteLeadMicros{0};
    std::vector<HeliosPointExt> frameBuffer;

    // These counters stay on the controller so reconnects can clear the noisy
    // short-term history without touching longer-lived manager state.
    std::size_t consecutiveStatusErrors = 0;
    std::size_t consecutiveWriteErrors = 0;
    std::chrono::steady_clock::time_point statusWarmupDeadline{};
};

} // namespace libera::idn
