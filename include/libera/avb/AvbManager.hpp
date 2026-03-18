#pragma once

#include "libera/System.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace libera::avb {

struct AvbAudioDeviceInfo {
    std::string uid;
    std::string label;
    std::uint32_t outputChannels = 0;
    std::uint32_t defaultPointRate = 0;
    bool pointRateMutable = false;
    std::vector<std::uint32_t> supportedPointRates;
};

struct AvbDeviceConfiguration {
    std::string deviceUid;
    std::uint32_t preferredPointRate = 0;
};

class AvbManager : public core::ControllerManagerBase {
public:
    AvbManager();
    ~AvbManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info) override;
    void closeAll() override;

    static std::vector<AvbAudioDeviceInfo> availableDevices();
    static std::vector<AvbDeviceConfiguration> configuredDevices();
    static void setConfiguredDevices(const std::vector<AvbDeviceConfiguration>& configs);
    static bool isDeviceEnabled(const std::string& deviceUid);
    static bool setDeviceEnabled(const std::string& deviceUid, bool enabled);
    static bool setPreferredPointRate(const std::string& deviceUid, std::uint32_t pointRateValue);

    static inline core::ControllerManagerRegistry registrar{
        [] { return std::make_unique<AvbManager>(); }
    };

private:
    static constexpr std::string_view typeName{"AVB"};
};

} // namespace libera::avb
