#pragma once

#include "AvbAudioHost.hpp"
#include "AvbDeviceRuntime.hpp"
#include "libera/avb/AvbController.hpp"
#include "libera/avb/AvbManager.hpp"
#include "libera/core/ControllerCache.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace libera::avb::detail {

// AVB keeps process-wide runtime and configuration state because several
// logical 8-channel controllers can share one audio-device runtime.
//
// This helper owns that shared state so AvbManager can stay focused on the
// public controller-manager interface instead of being both API surface and
// singleton state container.
class AvbBackendState {
public:
    using AudioHostFactory = std::function<std::shared_ptr<AudioHost>()>;

    static AvbBackendState& instance();

    // Tests can swap in a fake audio host so AVB discovery, configuration,
    // and shared-runtime behavior can be exercised without CoreAudio/RtAudio.
    // Passing an empty factory restores the default createAudioHost() path.
    static void setAudioHostFactoryForTesting(AudioHostFactory factory);
    static void resetForTesting();

    std::vector<std::unique_ptr<core::ControllerInfo>> discoverControllers();
    std::shared_ptr<AvbController> connectController(const AvbControllerInfo& info);
    void closeAll();

    std::vector<AvbAudioDeviceInfo> availableDevices();
    std::vector<AvbDeviceConfiguration> configuredDevices();
    std::vector<AvbControllerInfo> configuredControllers();
    void setConfiguredDevices(const std::vector<AvbDeviceConfiguration>& configs);
    bool isDeviceEnabled(const std::string& deviceUid);
    bool setDeviceEnabled(const std::string& deviceUid, bool enabled);
    bool setPreferredPointRate(const std::string& deviceUid,
                               std::uint32_t pointRateValue);
    bool halfXYOutputEnabled(const std::string& controllerId);
    std::vector<std::string> halfXYOutputControllers();
    void setHalfXYOutputControllers(const std::vector<std::string>& controllerIds);
    bool setHalfXYOutput(const std::string& controllerId, bool enabled);

private:
    AvbBackendState();
    void resetStateForTesting();

    std::unordered_map<std::string, AvbAudioDeviceInfo> availableDeviceMap();
    void applyHalfXYOutputSettingsLocked();
    std::shared_ptr<AvbDeviceRuntime> getOrCreateRuntimeLocked(
        const AvbControllerInfo& info,
        const AvbAudioDeviceInfo& device);

    std::shared_ptr<AudioHost> audioHost;
    std::mutex mutex;
    std::unordered_map<std::string, AvbDeviceConfiguration> configuredDevicesByUid;
    std::unordered_map<std::string, bool> halfXYOutputByControllerId;
    core::ControllerCache<std::string, AvbController> activeControllers;
    std::unordered_map<std::string, std::shared_ptr<AvbDeviceRuntime>> runtimesByDeviceUid;
};

} // namespace libera::avb::detail
