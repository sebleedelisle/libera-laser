#include "libera/avb/AvbManager.hpp"

#include "AvbBackendState.hpp"

namespace libera::avb {

namespace {

detail::AvbBackendState& backendState() {
    return detail::AvbBackendState::instance();
}

} // namespace

AvbManager::AvbManager() = default;

AvbManager::~AvbManager() {
    closeAll();
}

std::vector<std::unique_ptr<core::ControllerInfo>> AvbManager::discover() {
    return backendState().discoverControllers();
}

std::shared_ptr<core::LaserController>
AvbManager::connectController(const core::ControllerInfo& info) {
    const auto* avbInfo = dynamic_cast<const AvbControllerInfo*>(&info);
    if (!avbInfo) {
        return nullptr;
    }

    return backendState().connectController(*avbInfo);
}

void AvbManager::closeAll() {
    backendState().closeAll();
}

std::vector<AvbAudioDeviceInfo> AvbManager::availableDevices() {
    return backendState().availableDevices();
}

std::vector<AvbDeviceConfiguration> AvbManager::configuredDevices() {
    return backendState().configuredDevices();
}

std::vector<AvbControllerInfo> AvbManager::configuredControllers() {
    return backendState().configuredControllers();
}

void AvbManager::setConfiguredDevices(
    const std::vector<AvbDeviceConfiguration>& configs) {
    backendState().setConfiguredDevices(configs);
}

bool AvbManager::isDeviceEnabled(const std::string& deviceUid) {
    return backendState().isDeviceEnabled(deviceUid);
}

bool AvbManager::setDeviceEnabled(const std::string& deviceUid, bool enabled) {
    return backendState().setDeviceEnabled(deviceUid, enabled);
}

bool AvbManager::setPreferredPointRate(const std::string& deviceUid,
                                       std::uint32_t pointRateValue) {
    return backendState().setPreferredPointRate(deviceUid, pointRateValue);
}

bool AvbManager::halfXYOutputEnabled(const std::string& controllerId) {
    return backendState().halfXYOutputEnabled(controllerId);
}

std::vector<std::string> AvbManager::halfXYOutputControllers() {
    return backendState().halfXYOutputControllers();
}

void AvbManager::setHalfXYOutputControllers(
    const std::vector<std::string>& controllerIds) {
    backendState().setHalfXYOutputControllers(controllerIds);
}

bool AvbManager::setHalfXYOutput(const std::string& controllerId,
                                 bool enabled) {
    return backendState().setHalfXYOutput(controllerId, enabled);
}

} // namespace libera::avb
