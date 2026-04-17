#pragma once

#include "libera/avb/AvbControllerInfo.hpp"
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

class AvbManager : public core::AbstractControllerManager {
public:
    AvbManager();
    ~AvbManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info) override;
    void closeAll() override;

    static std::vector<AvbAudioDeviceInfo> availableDevices();
    static std::vector<AvbDeviceConfiguration> configuredDevices();
    // One logical AVB controller maps to one 8-channel bank on an enabled
    // audio interface. The returned IDs are the stable per-bank IDs used for
    // assignment, reconnect, and AVB-specific bank settings.
    static std::vector<AvbControllerInfo> configuredControllers();
    static void setConfiguredDevices(const std::vector<AvbDeviceConfiguration>& configs);
    static bool isDeviceEnabled(const std::string& deviceUid);
    static bool setDeviceEnabled(const std::string& deviceUid, bool enabled);
    static bool setPreferredPointRate(const std::string& deviceUid, std::uint32_t pointRateValue);
    // Half X/Y Output is an AVB-specific workaround for AVB-to-ILDA chains
    // where both ends fake a differential pair by grounding one side, which
    // otherwise doubles the effective scan size.
    static std::vector<std::string> halfXYOutputControllers();
    static void setHalfXYOutputControllers(const std::vector<std::string>& controllerIds);
    static bool halfXYOutputEnabled(const std::string& controllerId);
    static bool setHalfXYOutput(const std::string& controllerId, bool enabled);

    static core::ControllerManagerRegistry registrar;

private:
    static constexpr std::string_view typeName{"AVB"};
};

inline core::ControllerManagerRegistry AvbManager::registrar{
    [] { return std::make_unique<AvbManager>(); }
};

} // namespace libera::avb
