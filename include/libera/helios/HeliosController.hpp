#pragma once

#include "libera/core/LaserController.hpp"
#include "HeliosDac.h"

#include <atomic>
#include <memory>
#include <vector>

namespace libera::helios {

class HeliosController : public core::LaserController {
public:
    explicit HeliosController(std::shared_ptr<HeliosDac> sdk, unsigned int controllerIndex);
    ~HeliosController() override;

    void close();
    bool isConnected() const;

    void setPointRate(std::uint32_t pointRateValue) override;

    void setFramePointCount(std::size_t points);
    std::size_t framePointCount() const;

protected:
    void run() override;

private:
    std::shared_ptr<HeliosDac> sdk;
    unsigned int index = 0;
    std::atomic<std::size_t> targetFramePoints{1000};
    std::atomic<std::uint64_t> currentPointIndex{0};
    std::vector<HeliosPointExt> frameBuffer;

    // Simple counters used for log throttling and health diagnostics.
    std::size_t consecutiveStatusErrors = 0;
    std::size_t consecutiveWriteErrors = 0;
};

} // namespace libera::helios
