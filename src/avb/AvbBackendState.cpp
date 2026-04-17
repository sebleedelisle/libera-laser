#include "AvbBackendState.hpp"

#include "libera/avb/AvbControllerInfo.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace libera::avb::detail {
namespace {

constexpr std::uint32_t controllerChannelCount = 8;

std::string makeControllerId(const std::string& deviceUid,
                             std::uint32_t channelOffset) {
    return deviceUid + "::ch-" + std::to_string(channelOffset);
}

std::string makeControllerLabel(const std::string& deviceName,
                                std::uint32_t channelOffset,
                                std::uint32_t channelCount) {
    const auto firstChannel = channelOffset + 1;
    const auto lastChannel = channelOffset + channelCount;
    return deviceName + " " + std::to_string(firstChannel) + "-" +
           std::to_string(lastChannel);
}

bool isEligibleDevice(const AudioOutputDeviceInfo& device) {
    return !device.uid.empty() && device.outputChannels >= controllerChannelCount;
}

AvbAudioDeviceInfo toPublicDeviceInfo(const AudioOutputDeviceInfo& device) {
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

        uniqueConfigs.insert_or_assign(normalized.deviceUid,
                                       std::move(normalized));
    }

    std::vector<AvbDeviceConfiguration> normalizedConfigs;
    normalizedConfigs.reserve(uniqueConfigs.size());
    for (auto& [deviceUid, config] : uniqueConfigs) {
        (void)deviceUid;
        normalizedConfigs.push_back(std::move(config));
    }

    std::sort(normalizedConfigs.begin(),
              normalizedConfigs.end(),
              [](const AvbDeviceConfiguration& a,
                 const AvbDeviceConfiguration& b) {
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
                makeControllerLabel(device.label, channelOffset,
                                    controllerChannelCount),
                device.uid,
                device.label,
                channelOffset,
                controllerChannelCount,
                pointRates);
        }
    }

    return controllers;
}

} // namespace

AvbBackendState::AvbBackendState()
: audioHost(createAudioHost()) {}

AvbBackendState& AvbBackendState::instance() {
    // Discovery can still be running very late in process shutdown if the
    // owning app singleton is intentionally leaked. Keep AVB shared state
    // alive until process exit instead of participating in static teardown
    // order, which avoids use-after-destruction crashes on shutdown.
    static AvbBackendState& state = *new AvbBackendState();
    return state;
}

std::vector<std::unique_ptr<core::ControllerInfo>>
AvbBackendState::discoverControllers() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;

    for (const auto& controllerInfo : configuredControllers()) {
        results.emplace_back(std::make_unique<AvbControllerInfo>(controllerInfo));
    }

    return results;
}

std::shared_ptr<AvbController>
AvbBackendState::connectController(const AvbControllerInfo& info) {
    const auto devicesByUid = availableDeviceMap();
    const auto deviceIt = devicesByUid.find(info.deviceUid());
    if (deviceIt == devicesByUid.end()) {
        return nullptr;
    }

    std::uint32_t preferredPointRate = 0;
    bool halfXYOutput = false;
    {
        std::lock_guard lock(mutex);
        const auto configIt = configuredDevicesByUid.find(info.deviceUid());
        if (configIt != configuredDevicesByUid.end()) {
            preferredPointRate = configIt->second.preferredPointRate;
        }
        halfXYOutput =
            halfXYOutputByControllerId.find(info.idValue()) !=
            halfXYOutputByControllerId.end();
    }

    const auto pointRateValue =
        choosePointRate(&deviceIt->second, preferredPointRate);
    if (pointRateValue == 0) {
        return nullptr;
    }

    std::shared_ptr<AvbDeviceRuntime> runtime;
    auto controller = std::shared_ptr<AvbController>{};
    {
        std::lock_guard lock(mutex);
        runtime = getOrCreateRuntimeLocked(info, deviceIt->second);
        controller = activeControllers.getOrCreate(
            info.idValue(),
            [&info] {
                return std::make_shared<AvbController>(
                    info.idValue(),
                    info.deviceUid(),
                    info.channelOffset(),
                    info.channelCount());
            }).controller;
    }

    if (!runtime || !controller) {
        return nullptr;
    }

    controller->setHalfXYOutputEnabled(halfXYOutput);

    if (!runtime->open(pointRateValue)) {
        return nullptr;
    }

    runtime->attachController(info.channelOffset(), controller);
    return controller;
}

void AvbBackendState::closeAll() {
    std::unordered_map<std::string, std::shared_ptr<AvbController>> controllers;
    std::vector<std::shared_ptr<AvbDeviceRuntime>> runtimes;

    {
        std::lock_guard lock(mutex);
        controllers = activeControllers.snapshotAndClear();
        runtimes.reserve(runtimesByDeviceUid.size());
        for (auto& [deviceUid, runtime] : runtimesByDeviceUid) {
            (void)deviceUid;
            if (runtime) {
                runtimes.push_back(std::move(runtime));
            }
        }
        runtimesByDeviceUid.clear();
    }

    for (auto& runtime : runtimes) {
        runtime->close();
    }

    for (auto& [controllerId, controller] : controllers) {
        (void)controllerId;
        if (!controller) {
            continue;
        }
        controller->stopThread();
        controller->close();
    }
}

std::vector<AvbAudioDeviceInfo> AvbBackendState::availableDevices() {
    std::vector<AvbAudioDeviceInfo> devices;
    if (!audioHost) {
        return devices;
    }

    for (const auto& device : audioHost->listOutputDevices()) {
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

std::vector<AvbDeviceConfiguration> AvbBackendState::configuredDevices() {
    std::vector<AvbDeviceConfiguration> configs;

    std::lock_guard lock(mutex);
    configs.reserve(configuredDevicesByUid.size());
    for (const auto& [deviceUid, config] : configuredDevicesByUid) {
        (void)deviceUid;
        configs.push_back(config);
    }

    std::sort(configs.begin(),
              configs.end(),
              [](const AvbDeviceConfiguration& a,
                 const AvbDeviceConfiguration& b) {
                  return a.deviceUid < b.deviceUid;
              });
    return configs;
}

std::vector<AvbControllerInfo> AvbBackendState::configuredControllers() {
    return buildConfiguredControllers(availableDevices(), configuredDevices());
}

void AvbBackendState::setConfiguredDevices(
    const std::vector<AvbDeviceConfiguration>& configs) {
    const auto devicesByUid = availableDeviceMap();
    const auto normalizedConfigs =
        normalizeConfigurations(configs, devicesByUid);

    std::unordered_map<std::string, AvbDeviceConfiguration> newConfigsByUid;
    for (const auto& config : normalizedConfigs) {
        newConfigsByUid.insert_or_assign(config.deviceUid, config);
    }

    std::vector<std::shared_ptr<AvbDeviceRuntime>> runtimesToClose;
    std::vector<std::pair<std::shared_ptr<AvbDeviceRuntime>, std::uint32_t>>
        runtimesToReopen;

    {
        std::lock_guard lock(mutex);

        for (auto it = runtimesByDeviceUid.begin();
             it != runtimesByDeviceUid.end();) {
            const auto configIt = newConfigsByUid.find(it->first);
            if (configIt == newConfigsByUid.end()) {
                if (it->second) {
                    runtimesToClose.push_back(it->second);
                }
                it = runtimesByDeviceUid.erase(it);
                continue;
            }

            const auto availableIt = devicesByUid.find(it->first);
            const auto desiredPointRate =
                choosePointRate(availableIt != devicesByUid.end()
                                    ? &availableIt->second
                                    : nullptr,
                                configIt->second.preferredPointRate);
            if (desiredPointRate > 0 &&
                it->second &&
                it->second->currentPointRate() > 0 &&
                desiredPointRate != it->second->currentPointRate()) {
                runtimesToReopen.emplace_back(it->second, desiredPointRate);
            }
            ++it;
        }

        configuredDevicesByUid = std::move(newConfigsByUid);
    }

    for (auto& runtime : runtimesToClose) {
        runtime->close();
    }

    for (auto& [runtime, pointRateValue] : runtimesToReopen) {
        runtime->reopen(pointRateValue);
    }
}

bool AvbBackendState::isDeviceEnabled(const std::string& deviceUid) {
    if (deviceUid.empty()) {
        return false;
    }

    std::lock_guard lock(mutex);
    return configuredDevicesByUid.find(deviceUid) != configuredDevicesByUid.end();
}

bool AvbBackendState::setDeviceEnabled(const std::string& deviceUid,
                                       bool enabled) {
    if (deviceUid.empty()) {
        return false;
    }

    auto configs = configuredDevices();
    auto it = std::find_if(configs.begin(),
                           configs.end(),
                           [&deviceUid](const AvbDeviceConfiguration& config) {
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

bool AvbBackendState::setPreferredPointRate(const std::string& deviceUid,
                                            std::uint32_t pointRateValue) {
    if (deviceUid.empty()) {
        return false;
    }

    auto configs = configuredDevices();
    auto it = std::find_if(configs.begin(),
                           configs.end(),
                           [&deviceUid](const AvbDeviceConfiguration& config) {
                               return config.deviceUid == deviceUid;
                           });
    if (it == configs.end()) {
        return false;
    }

    it->preferredPointRate = pointRateValue;
    setConfiguredDevices(configs);
    return true;
}

bool AvbBackendState::halfXYOutputEnabled(const std::string& controllerId) {
    if (controllerId.empty()) {
        return false;
    }

    std::lock_guard lock(mutex);
    const auto it = halfXYOutputByControllerId.find(controllerId);
    return it != halfXYOutputByControllerId.end() && it->second;
}

std::vector<std::string> AvbBackendState::halfXYOutputControllers() {
    std::vector<std::string> controllerIds;

    std::lock_guard lock(mutex);
    controllerIds.reserve(halfXYOutputByControllerId.size());
    for (const auto& [controllerId, enabled] : halfXYOutputByControllerId) {
        if (!enabled) {
            continue;
        }
        controllerIds.push_back(controllerId);
    }

    std::sort(controllerIds.begin(), controllerIds.end());
    return controllerIds;
}

void AvbBackendState::setHalfXYOutputControllers(
    const std::vector<std::string>& controllerIds) {
    std::unordered_set<std::string> uniqueControllerIds;
    uniqueControllerIds.reserve(controllerIds.size());
    for (const auto& controllerId : controllerIds) {
        if (!controllerId.empty()) {
            uniqueControllerIds.insert(controllerId);
        }
    }

    std::lock_guard lock(mutex);
    halfXYOutputByControllerId.clear();
    for (const auto& controllerId : uniqueControllerIds) {
        halfXYOutputByControllerId.insert_or_assign(controllerId, true);
    }
    applyHalfXYOutputSettingsLocked();
}

bool AvbBackendState::setHalfXYOutput(const std::string& controllerId,
                                      bool enabled) {
    if (controllerId.empty()) {
        return false;
    }

    std::lock_guard lock(mutex);
    if (enabled) {
        halfXYOutputByControllerId.insert_or_assign(controllerId, true);
    } else {
        halfXYOutputByControllerId.erase(controllerId);
    }
    applyHalfXYOutputSettingsLocked();
    return true;
}

std::unordered_map<std::string, AvbAudioDeviceInfo>
AvbBackendState::availableDeviceMap() {
    std::unordered_map<std::string, AvbAudioDeviceInfo> devicesByUid;
    for (const auto& device : availableDevices()) {
        devicesByUid.insert_or_assign(device.uid, device);
    }
    return devicesByUid;
}

void AvbBackendState::applyHalfXYOutputSettingsLocked() {
    const auto liveControllers = activeControllers.snapshot();
    for (const auto& [controllerId, controller] : liveControllers) {
        const bool halfXYOutput =
            halfXYOutputByControllerId.find(controllerId) !=
            halfXYOutputByControllerId.end();
        controller->setHalfXYOutputEnabled(halfXYOutput);
    }
}

std::shared_ptr<AvbDeviceRuntime> AvbBackendState::getOrCreateRuntimeLocked(
    const AvbControllerInfo& info,
    const AvbAudioDeviceInfo& device) {
    const auto runtimeIt = runtimesByDeviceUid.find(info.deviceUid());
    if (runtimeIt != runtimesByDeviceUid.end()) {
        return runtimeIt->second;
    }

    AudioOutputDeviceInfo runtimeDeviceInfo;
    runtimeDeviceInfo.uid = device.uid;
    runtimeDeviceInfo.label = device.label;
    runtimeDeviceInfo.outputChannels = device.outputChannels;
    runtimeDeviceInfo.defaultPointRate = device.defaultPointRate;
    runtimeDeviceInfo.pointRateMutable = device.pointRateMutable;
    runtimeDeviceInfo.supportedPointRates = device.supportedPointRates;

    auto runtime = std::make_shared<AvbDeviceRuntime>(audioHost,
                                                      std::move(runtimeDeviceInfo));
    runtimesByDeviceUid.insert_or_assign(info.deviceUid(), runtime);
    return runtime;
}

} // namespace libera::avb::detail
