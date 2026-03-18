#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace libera::avb::detail {

struct AudioOutputDeviceInfo {
    std::string uid;
    std::string label;
    std::uint32_t outputChannels = 0;
    std::uint32_t defaultPointRate = 0;
    bool pointRateMutable = false;
    std::vector<std::uint32_t> supportedPointRates;
};

using AudioOutputCallback =
    std::function<void(float* output,
                       std::uint32_t frameCount,
                       std::uint32_t channelCount,
                       std::chrono::steady_clock::duration outputLeadTime)>;

class AudioOutputStream {
public:
    virtual ~AudioOutputStream() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual std::uint32_t pointRate() const = 0;
    virtual std::uint32_t channelCount() const = 0;
};

class AudioHost {
public:
    virtual ~AudioHost() = default;

    virtual std::vector<AudioOutputDeviceInfo> listOutputDevices() = 0;
    virtual std::unique_ptr<AudioOutputStream> openOutputStream(
        const AudioOutputDeviceInfo& device,
        std::uint32_t pointRateValue,
        AudioOutputCallback callback) = 0;
};

std::shared_ptr<AudioHost> createAudioHost();

} // namespace libera::avb::detail
