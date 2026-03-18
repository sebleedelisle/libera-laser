#pragma once

#include "libera/core/LaserController.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace libera::avb {

namespace detail {
class AvbDeviceRuntime;
}

class AvbController : public core::LaserController {
public:
    AvbController(std::string stableId,
                  std::string deviceUid,
                  std::uint32_t channelOffset,
                  std::uint32_t channelCount);
    ~AvbController() override;

    const std::string& stableId() const { return stableControllerId; }
    const std::string& deviceUid() const { return deviceUidValue; }
    std::uint32_t channelOffset() const { return channelOffsetValue; }
    std::uint32_t channelCount() const { return channelCountValue; }

    bool isConnected() const;
    void close();

    void setPointRate(std::uint32_t pointRateValue) override;

protected:
    void run() override;

private:
    friend class detail::AvbDeviceRuntime;

    void attachToRuntime(std::uint32_t pointRateValue);
    void detachFromRuntime();
    void renderInterleavedBlock(float* output,
                                std::size_t frameCount,
                                std::size_t totalChannelCount,
                                std::chrono::steady_clock::time_point estimatedFirstRenderTime);

    std::string stableControllerId;
    std::string deviceUidValue;
    std::uint32_t channelOffsetValue = 0;
    std::uint32_t channelCountValue = 8;
    std::atomic<std::uint64_t> currentPointIndex{0};
};

} // namespace libera::avb
