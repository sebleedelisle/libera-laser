#include "libera/avb/AvbManager.hpp"

#include "AvbAudioHost.hpp"
#include "AvbDeviceRuntime.hpp"
#include "libera/avb/AvbController.hpp"
#include "libera/avb/AvbControllerInfo.hpp"
#include "libera/core/ActiveControllerMap.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace libera::avb {
namespace {

constexpr std::uint32_t controllerChannelCount = 8;

struct SharedState {
    std::shared_ptr<detail::AudioHost> audioHost = detail::createAudioHost();
    std::mutex mutex;
    std::unordered_map<std::string, AvbDeviceConfiguration> configuredDevicesByUid;
    std::unordered_map<std::string, bool> halfXYOutputByControllerId;
    std::unordered_map<std::string, std::weak_ptr<AvbController>> activeControllers;
    std::unordered_map<std::string, std::shared_ptr<detail::AvbDeviceRuntime>> runtimesByDeviceUid;
};

SharedState& sharedState() {
    static SharedState state;
    return state;
}

std::string makeControllerId(const std::string& deviceUid, std::uint32_t channelOffset) {
    return deviceUid + "::ch-" + std::to_string(channelOffset);
}

std::string makeControllerLabel(const std::string& deviceName,
                                std::uint32_t channelOffset,
                                std::uint32_t channelCount) {
    const auto firstChannel = channelOffset + 1;
    const auto lastChannel = channelOffset + channelCount;
    return deviceName + " " + std::to_string(firstChannel) + "-" + std::to_string(lastChannel);
}

bool isEligibleDevice(const detail::AudioOutputDeviceInfo& device) {
    return !device.uid.empty() && device.outputChannels >= controllerChannelCount;
}

AvbAudioDeviceInfo toPublicDeviceInfo(const detail::AudioOutputDeviceInfo& device) {
    AvbAudioDeviceInfo info;
    info.uid = device.uid;
    info.label = device.label;
    info.outputChannels = device.outputChannels;
    info.defaultPointRate = device.defaultPointRate;
    info.pointRateMutable = device.pointRateMutable;
    info.supportedPointRates = device.supportedPointRates;
    return info;
}

std::uint32_t choosePointRate(const AvbAudioDeviceInfo* device,
                              std::uint32_t preferredPointRate) {
    if (device == nullptr) {
        return preferredPointRate;
    }

    if (preferredPointRate > 0) {
        if (device->supportedPointRates.empty()) {
            return preferredPointRate;
        }

        const auto it = std::find(device->supportedPointRates.begin(),
                                  device->supportedPointRates.end(),
                                  preferredPointRate);
        if (it != device->supportedPointRates.end()) {
            return preferredPointRate;
        }
    }

    if (device->defaultPointRate > 0) {
        return device->defaultPointRate;
    }

    if (!device->supportedPointRates.empty()) {
        return device->supportedPointRates.front();
    }

    return preferredPointRate;
}

std::unordered_map<std::string, AvbAudioDeviceInfo> availableDeviceMap() {
    std::unordered_map<std::string, AvbAudioDeviceInfo> devicesByUid;
    for (const auto& device : AvbManager::availableDevices()) {
        devicesByUid.insert_or_assign(device.uid, device);
    }
    return devicesByUid;
}

std::vector<AvbDeviceConfiguration> normalizeConfigurations(
    const std::vector<AvbDeviceConfiguration>& configs,
    const std::unordered_map<std::string, AvbAudioDeviceInfo>& devicesByUid) {
    std::unordered_map<std::string, AvbDeviceConfiguration> uniqueConfigs;
    for (const auto& config : configs) {
        if (config.deviceUid.empty()) {
            continue;
        }

        auto normalized = config;
        const auto availableIt = devicesByUid.find(config.deviceUid);
        if (availableIt != devicesByUid.end()) {
            normalized.preferredPointRate =
                choosePointRate(&availableIt->second, config.preferredPointRate);
        }

        uniqueConfigs.insert_or_assign(normalized.deviceUid, std::move(normalized));
    }

    std::vector<AvbDeviceConfiguration> normalizedConfigs;
    normalizedConfigs.reserve(uniqueConfigs.size());
    for (auto& [deviceUid, config] : uniqueConfigs) {
        (void)deviceUid;
        normalizedConfigs.push_back(std::move(config));
    }

    std::sort(normalizedConfigs.begin(),
              normalizedConfigs.end(),
              [](const AvbDeviceConfiguration& a, const AvbDeviceConfiguration& b) {
                  return a.deviceUid < b.deviceUid;
              });

    return normalizedConfigs;
}

AvbControllerInfo::PointRateCapabilities buildPointRateCapabilities(
    const AvbAudioDeviceInfo& device) {
    AvbControllerInfo::PointRateCapabilities capabilities;
    capabilities.pointRateMutable = device.pointRateMutable;
    capabilities.defaultPointRate = device.defaultPointRate;
    capabilities.supportedPointRates = device.supportedPointRates;

    if (!capabilities.supportedPointRates.empty()) {
        const auto [minIt, maxIt] =
            std::minmax_element(capabilities.supportedPointRates.begin(),
                                capabilities.supportedPointRates.end());
        capabilities.minPointRate = *minIt;
        capabilities.maxPointRate = *maxIt;
    } else {
        capabilities.minPointRate = device.defaultPointRate;
        capabilities.maxPointRate = device.defaultPointRate;
    }

    return capabilities;
}

std::vector<AvbControllerInfo> buildConfiguredControllers(
    const std::vector<AvbAudioDeviceInfo>& devices,
    const std::vector<AvbDeviceConfiguration>& configs) {
    std::vector<AvbControllerInfo> controllers;

    std::unordered_map<std::string, AvbDeviceConfiguration> configsByUid;
    for (const auto& config : configs) {
        configsByUid.insert_or_assign(config.deviceUid, config);
    }

    for (const auto& device : devices) {
        if (configsByUid.find(device.uid) == configsByUid.end()) {
            continue;
        }

        const auto pointRates = buildPointRateCapabilities(device);
        const auto bankCount = device.outputChannels / controllerChannelCount;
        for (std::uint32_t bankIndex = 0; bankIndex < bankCount; ++bankIndex) {
            const auto channelOffset = bankIndex * controllerChannelCount;
            controllers.emplace_back(
                makeControllerId(device.uid, channelOffset),
                makeControllerLabel(device.label, channelOffset, controllerChannelCount),
                device.uid,
                device.label,
                channelOffset,
                controllerChannelCount,
                pointRates);
        }
    }

    return controllers;
}

void applyHalfXYOutputSettingsLocked(SharedState& state) {
    for (auto it = state.activeControllers.begin(); it != state.activeControllers.end();) {
        auto controller = it->second.lock();
        if (!controller) {
            it = state.activeControllers.erase(it);
            continue;
        }

        const bool halfXYOutput =
            state.halfXYOutputByControllerId.find(it->first) != state.halfXYOutputByControllerId.end();
        controller->setHalfXYOutputEnabled(halfXYOutput);
        ++it;
    }
}

} // namespace

AvbManager::AvbManager() = default;

AvbManager::~AvbManager() {
    closeAll();
}

std::vector<std::unique_ptr<core::ControllerInfo>> AvbManager::discover() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;

    for (const auto& controllerInfo : configuredControllers()) {
        results.emplace_back(std::make_unique<AvbControllerInfo>(controllerInfo));
    }

    return results;
}

std::shared_ptr<core::LaserController>
AvbManager::connectController(const core::ControllerInfo& info) {
    const auto* avbInfo = dynamic_cast<const AvbControllerInfo*>(&info);
    if (!avbInfo) {
        return nullptr;
    }

    const auto devicesByUid = availableDeviceMap();
    const auto deviceIt = devicesByUid.find(avbInfo->deviceUid());
    if (deviceIt == devicesByUid.end()) {
        return nullptr;
    }

    std::uint32_t preferredPointRate = 0;
    {
        std::lock_guard lock(sharedState().mutex);
        const auto configIt = sharedState().configuredDevicesByUid.find(avbInfo->deviceUid());
        if (configIt != sharedState().configuredDevicesByUid.end()) {
            preferredPointRate = configIt->second.preferredPointRate;
        }
    }

    const auto pointRateValue = choosePointRate(&deviceIt->second, preferredPointRate);
    if (pointRateValue == 0) {
        return nullptr;
    }

    std::shared_ptr<detail::AvbDeviceRuntime> runtime;
    std::shared_ptr<AvbController> controller;
    {
        auto& state = sharedState();
        std::lock_guard lock(state.mutex);

        auto runtimeIt = state.runtimesByDeviceUid.find(avbInfo->deviceUid());
        if (runtimeIt != state.runtimesByDeviceUid.end()) {
            runtime = runtimeIt->second;
        } else {
            detail::AudioOutputDeviceInfo runtimeDeviceInfo;
            runtimeDeviceInfo.uid = deviceIt->second.uid;
            runtimeDeviceInfo.label = deviceIt->second.label;
            runtimeDeviceInfo.outputChannels = deviceIt->second.outputChannels;
            runtimeDeviceInfo.defaultPointRate = deviceIt->second.defaultPointRate;
            runtimeDeviceInfo.pointRateMutable = deviceIt->second.pointRateMutable;
            runtimeDeviceInfo.supportedPointRates = deviceIt->second.supportedPointRates;

            runtime = std::make_shared<detail::AvbDeviceRuntime>(state.audioHost, std::move(runtimeDeviceInfo));
            state.runtimesByDeviceUid.insert_or_assign(avbInfo->deviceUid(), runtime);
        }

        controller = core::getOrCreateActiveController(
            state.activeControllers,
            avbInfo->idValue(),
            [avbInfo] {
                return std::make_shared<AvbController>(
                    avbInfo->idValue(),
                    avbInfo->deviceUid(),
                    avbInfo->channelOffset(),
                    avbInfo->channelCount());
            });
    }

    if (!runtime || !controller) {
        return nullptr;
    }

    controller->setHalfXYOutputEnabled(halfXYOutputEnabled(avbInfo->idValue()));

    if (!runtime->open(pointRateValue)) {
        return nullptr;
    }

    runtime->attachController(avbInfo->channelOffset(), controller);
    return controller;
}

void AvbManager::closeAll() {
    std::unordered_map<std::string, std::shared_ptr<AvbController>> controllers;
    std::vector<std::shared_ptr<detail::AvbDeviceRuntime>> runtimes;

    {
        auto& state = sharedState();
        std::lock_guard lock(state.mutex);
        controllers = core::snapshotActiveControllersAndClear(state.activeControllers);
        runtimes.reserve(state.runtimesByDeviceUid.size());
        for (auto& [deviceUid, runtime] : state.runtimesByDeviceUid) {
            (void)deviceUid;
            if (runtime) {
                runtimes.push_back(std::move(runtime));
            }
        }
        state.runtimesByDeviceUid.clear();
    }

    for (auto& runtime : runtimes) {
        runtime->close();
    }

    for (auto& [controllerId, controller] : controllers) {
        (void)controllerId;
        if (!controller) {
            continue;
        }
        controller->stop();
        controller->close();
    }
}

std::vector<AvbAudioDeviceInfo> AvbManager::availableDevices() {
    std::vector<AvbAudioDeviceInfo> devices;

    auto& state = sharedState();
    if (!state.audioHost) {
        return devices;
    }

    for (const auto& device : state.audioHost->listOutputDevices()) {
        if (!isEligibleDevice(device)) {
            continue;
        }
        devices.push_back(toPublicDeviceInfo(device));
    }

    std::sort(devices.begin(),
              devices.end(),
              [](const AvbAudioDeviceInfo& a, const AvbAudioDeviceInfo& b) {
                  if (a.label != b.label) {
                      return a.label < b.label;
                  }
                  return a.uid < b.uid;
              });

    return devices;
}

std::vector<AvbDeviceConfiguration> AvbManager::configuredDevices() {
    std::vector<AvbDeviceConfiguration> configs;

    auto& state = sharedState();
    std::lock_guard lock(state.mutex);
    configs.reserve(state.configuredDevicesByUid.size());
    for (const auto& [deviceUid, config] : state.configuredDevicesByUid) {
        (void)deviceUid;
        configs.push_back(config);
    }

    std::sort(configs.begin(),
              configs.end(),
              [](const AvbDeviceConfiguration& a, const AvbDeviceConfiguration& b) {
                  return a.deviceUid < b.deviceUid;
              });
    return configs;
}

std::vector<AvbControllerInfo> AvbManager::configuredControllers() {
    return buildConfiguredControllers(availableDevices(), configuredDevices());
}

void AvbManager::setConfiguredDevices(const std::vector<AvbDeviceConfiguration>& configs) {
    const auto devicesByUid = availableDeviceMap();
    const auto normalizedConfigs = normalizeConfigurations(configs, devicesByUid);

    std::unordered_map<std::string, AvbDeviceConfiguration> newConfigsByUid;
    for (const auto& config : normalizedConfigs) {
        newConfigsByUid.insert_or_assign(config.deviceUid, config);
    }

    std::vector<std::shared_ptr<detail::AvbDeviceRuntime>> runtimesToClose;
    std::vector<std::pair<std::shared_ptr<detail::AvbDeviceRuntime>, std::uint32_t>> runtimesToReopen;

    auto& state = sharedState();
    {
        std::lock_guard lock(state.mutex);

        for (auto it = state.runtimesByDeviceUid.begin(); it != state.runtimesByDeviceUid.end();) {
            const auto configIt = newConfigsByUid.find(it->first);
            if (configIt == newConfigsByUid.end()) {
                if (it->second) {
                    runtimesToClose.push_back(it->second);
                }
                it = state.runtimesByDeviceUid.erase(it);
                continue;
            }

            const auto availableIt = devicesByUid.find(it->first);
            const auto desiredPointRate =
                choosePointRate(availableIt != devicesByUid.end() ? &availableIt->second : nullptr,
                                configIt->second.preferredPointRate);
            if (desiredPointRate > 0 &&
                it->second &&
                it->second->currentPointRate() > 0 &&
                desiredPointRate != it->second->currentPointRate()) {
                runtimesToReopen.emplace_back(it->second, desiredPointRate);
            }
            ++it;
        }

        state.configuredDevicesByUid = std::move(newConfigsByUid);
    }

    for (auto& runtime : runtimesToClose) {
        runtime->close();
    }

    for (auto& [runtime, pointRateValue] : runtimesToReopen) {
        runtime->reopen(pointRateValue);
    }
}

bool AvbManager::isDeviceEnabled(const std::string& deviceUid) {
    if (deviceUid.empty()) {
        return false;
    }

    auto& state = sharedState();
    std::lock_guard lock(state.mutex);
    return state.configuredDevicesByUid.find(deviceUid) != state.configuredDevicesByUid.end();
}

bool AvbManager::setDeviceEnabled(const std::string& deviceUid, bool enabled) {
    if (deviceUid.empty()) {
        return false;
    }

    auto configs = configuredDevices();
    auto it = std::find_if(configs.begin(), configs.end(), [&deviceUid](const AvbDeviceConfiguration& config) {
        return config.deviceUid == deviceUid;
    });

    if (enabled) {
        const auto devicesByUid = availableDeviceMap();
        const auto deviceIt = devicesByUid.find(deviceUid);
        if (deviceIt == devicesByUid.end()) {
            return false;
        }

        if (it == configs.end()) {
            configs.push_back(AvbDeviceConfiguration{
                deviceUid,
                choosePointRate(&deviceIt->second, 0)});
        }
    } else if (it != configs.end()) {
        configs.erase(it);
    }

    setConfiguredDevices(configs);
    return true;
}

bool AvbManager::setPreferredPointRate(const std::string& deviceUid, std::uint32_t pointRateValue) {
    if (deviceUid.empty()) {
        return false;
    }

    auto configs = configuredDevices();
    auto it = std::find_if(configs.begin(), configs.end(), [&deviceUid](const AvbDeviceConfiguration& config) {
        return config.deviceUid == deviceUid;
    });
    if (it == configs.end()) {
        return false;
    }

    it->preferredPointRate = pointRateValue;
    setConfiguredDevices(configs);
    return true;
}

bool AvbManager::halfXYOutputEnabled(const std::string& controllerId) {
    if (controllerId.empty()) {
        return false;
    }

    auto& state = sharedState();
    std::lock_guard lock(state.mutex);
    const auto it = state.halfXYOutputByControllerId.find(controllerId);
    return it != state.halfXYOutputByControllerId.end() && it->second;
}

std::vector<std::string> AvbManager::halfXYOutputControllers() {
    std::vector<std::string> controllerIds;

    auto& state = sharedState();
    std::lock_guard lock(state.mutex);
    controllerIds.reserve(state.halfXYOutputByControllerId.size());
    for (const auto& [controllerId, enabled] : state.halfXYOutputByControllerId) {
        if (!enabled) {
            continue;
        }
        controllerIds.push_back(controllerId);
    }

    std::sort(controllerIds.begin(), controllerIds.end());
    return controllerIds;
}

void AvbManager::setHalfXYOutputControllers(const std::vector<std::string>& controllerIds) {
    std::unordered_set<std::string> uniqueControllerIds;
    uniqueControllerIds.reserve(controllerIds.size());
    for (const auto& controllerId : controllerIds) {
        if (!controllerId.empty()) {
            uniqueControllerIds.insert(controllerId);
        }
    }

    auto& state = sharedState();
    std::lock_guard lock(state.mutex);
    state.halfXYOutputByControllerId.clear();
    for (const auto& controllerId : uniqueControllerIds) {
        state.halfXYOutputByControllerId.insert_or_assign(controllerId, true);
    }
    applyHalfXYOutputSettingsLocked(state);
}

bool AvbManager::setHalfXYOutput(const std::string& controllerId, bool enabled) {
    if (controllerId.empty()) {
        return false;
    }

    auto& state = sharedState();
    std::lock_guard lock(state.mutex);

    if (enabled) {
        state.halfXYOutputByControllerId.insert_or_assign(controllerId, true);
    } else {
        state.halfXYOutputByControllerId.erase(controllerId);
    }
    applyHalfXYOutputSettingsLocked(state);

    return true;
}

} // namespace libera::avb
