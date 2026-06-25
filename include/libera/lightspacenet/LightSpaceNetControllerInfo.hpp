#pragma once

#include "libera/System.hpp"
#include "libera/lightspacenet/LightSpaceNetConfig.hpp"
#include "libera/lightspacenet/LightSpaceNetStatus.hpp"

#include <string>
#include <utility>

namespace libera::lightspacenet {

class LightSpaceNetControllerInfo : public core::ControllerInfo {
public:
    static constexpr std::string_view controllerType() {
        return "LightSpaceNet";
    }

    explicit LightSpaceNetControllerInfo(LightSpaceNetStatus statusValue)
    : core::ControllerInfo(controllerType(),
                           statusValue.stableId(),
                           statusValue.displayLabel(),
                           LightSpaceNetConfig::MAX_POINT_RATE,
                           core::ControllerInfo::NetworkInfo{
                               statusValue.ipAddress,
                               LightSpaceNetConfig::NETWORK_PORT})
    , cachedStatus(std::move(statusValue)) {}

    const LightSpaceNetStatus& status() const { return cachedStatus; }
    const std::string& ipAddress() const { return cachedStatus.ipAddress; }
    const std::string& macAddress() const { return cachedStatus.macAddressString; }
    void setPreferredPointRate(std::uint32_t pointRate) {
        preferredPointRateValue = pointRate;
    }
    std::uint32_t preferredPointRate() const {
        return preferredPointRateValue;
    }

private:
    LightSpaceNetStatus cachedStatus;
    std::uint32_t preferredPointRateValue = 0;
};

} // namespace libera::lightspacenet
