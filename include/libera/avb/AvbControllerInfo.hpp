#pragma once

#include "libera/System.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace libera::avb {

class AvbControllerInfo : public core::ControllerInfo {
public:
    struct PointRateCapabilities {
        bool pointRateMutable = false;
        std::uint32_t defaultPointRate = 0;
        std::uint32_t minPointRate = 0;
        std::uint32_t maxPointRate = 0;
        std::vector<std::uint32_t> supportedPointRates;
    };

    AvbControllerInfo(std::string id,
                      std::string label,
                      std::string deviceUid,
                      std::string deviceName,
                      std::uint32_t channelOffset,
                      std::uint32_t channelCount,
                      PointRateCapabilities pointRates)
    : core::ControllerInfo("AVB",
                           std::move(id),
                           std::move(label),
                           std::max(pointRates.maxPointRate, pointRates.defaultPointRate))
    , deviceUidValue(std::move(deviceUid))
    , deviceNameValue(std::move(deviceName))
    , channelOffsetValue(channelOffset)
    , channelCountValue(channelCount)
    , pointRateCapabilities(std::move(pointRates)) {}

    const std::string& deviceUid() const { return deviceUidValue; }
    const std::string& deviceName() const { return deviceNameValue; }
    std::uint32_t channelOffset() const { return channelOffsetValue; }
    std::uint32_t channelCount() const { return channelCountValue; }
    const PointRateCapabilities& pointRates() const { return pointRateCapabilities; }

private:
    std::string deviceUidValue;
    std::string deviceNameValue;
    std::uint32_t channelOffsetValue = 0;
    std::uint32_t channelCountValue = 8;
    PointRateCapabilities pointRateCapabilities;
};

} // namespace libera::avb
