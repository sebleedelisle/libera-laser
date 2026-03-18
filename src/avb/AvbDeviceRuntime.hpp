#pragma once

#include "AvbAudioHost.hpp"
#include "libera/avb/AvbController.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace libera::avb::detail {

class AvbDeviceRuntime : public std::enable_shared_from_this<AvbDeviceRuntime> {
public:
    AvbDeviceRuntime(std::shared_ptr<AudioHost> audioHost,
                     AudioOutputDeviceInfo deviceInfo);
    ~AvbDeviceRuntime();

    const std::string& deviceUid() const { return deviceInfoValue.uid; }
    std::uint32_t currentPointRate() const {
        return currentPointRateValue.load(std::memory_order_relaxed);
    }
    std::uint32_t channelCount() const { return deviceInfoValue.outputChannels; }

    bool open(std::uint32_t pointRateValue);
    bool reopen(std::uint32_t pointRateValue);
    void close();

    void attachController(std::uint32_t channelOffset,
                          const std::shared_ptr<AvbController>& controller);
    void detachController(std::uint32_t channelOffset);

private:
    struct BankState {
        std::weak_ptr<AvbController> controller;
    };

    void handleAudioCallback(float* output,
                             std::uint32_t frameCount,
                             std::uint32_t channelCount,
                             std::chrono::steady_clock::duration outputLeadTime);
    void notifyControllersAttached(std::uint32_t pointRateValue);
    void notifyControllersDetached();

    std::shared_ptr<AudioHost> audioHost;
    AudioOutputDeviceInfo deviceInfoValue;
    std::unique_ptr<AudioOutputStream> stream;
    std::atomic<std::uint32_t> currentPointRateValue{0};

    std::mutex banksMutex;
    std::unordered_map<std::uint32_t, BankState> banksByOffset;
};

} // namespace libera::avb::detail
