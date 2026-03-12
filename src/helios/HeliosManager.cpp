#include "libera/helios/HeliosManager.hpp"

#include "libera/log/Log.hpp"

#include <algorithm>

namespace libera::helios {
namespace {

std::string makeFallbackLabel(unsigned int index) {
    return "Helios " + std::to_string(index);
}

} // namespace

HeliosManager::HeliosManager() {
    sdk = std::make_shared<HeliosDac>();
}

HeliosManager::~HeliosManager() {
    closeAll();
}

void HeliosManager::openIfNeeded() {
    if (!sdk) {
        sdk = std::make_shared<HeliosDac>();
    }

    if (opened) {
        return;
    }

    const int count = sdk->OpenDevicesOnlyUsb();
    opened = true;
    deviceCount = count > 0 ? static_cast<std::size_t>(count) : 0;
}

std::size_t HeliosManager::refreshDeviceCount(bool allowRescan) {
    openIfNeeded();
    if (!sdk) {
        deviceCount = 0;
        return deviceCount;
    }

    if (!allowRescan) {
        return deviceCount;
    }

    const int count = sdk->ReScanDevicesOnlyUsb();
    if (count > 0) {
        deviceCount = static_cast<std::size_t>(count);
    }
    return deviceCount;
}

std::vector<std::unique_ptr<core::DacInfo>> HeliosManager::discover() {
    std::vector<std::unique_ptr<core::DacInfo>> results;

    bool hasActive = false;
    {
        std::lock_guard lock(activeMutex);
        for (auto it = activeDevices.begin(); it != activeDevices.end();) {
            if (it->second.expired()) {
                it = activeDevices.erase(it);
            } else {
                hasActive = true;
                ++it;
            }
        }
    }

    const auto count = refreshDeviceCount(!hasActive);
    if (!sdk || count == 0) {
        return results;
    }

    results.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
        const int closed = sdk->GetIsClosed(index);
        if (closed > 0) {
            continue;
        }

        char name[32] = {};
        std::string label;
        if (sdk->GetName(index, name) == HELIOS_SUCCESS) {
            label = name;
        } else {
            label = makeFallbackLabel(index);
        }

        const int isUsb = sdk->GetIsUsb(index);
        const bool usbDevice = isUsb == 1;
        const std::uint32_t maxRate = usbDevice ? HELIOS_MAX_PPS : HELIOS_MAX_PPS_IDN;
        const int firmware = sdk->GetFirmwareVersion(index);
        std::string id = "helios-" + std::to_string(index);

        results.emplace_back(std::make_unique<HeliosDeviceInfo>(
            std::move(id),
            std::move(label),
            maxRate,
            index,
            usbDevice,
            firmware));
    }

    return results;
}

std::shared_ptr<core::LaserController>
HeliosManager::getAndConnectToDac(const core::DacInfo& info) {
    const auto* heliosInfo = dynamic_cast<const HeliosDeviceInfo*>(&info);
    if (!heliosInfo) {
        return nullptr;
    }

    std::shared_ptr<HeliosDevice> device;
    {
        std::lock_guard lock(activeMutex);
        auto it = activeDevices.find(heliosInfo->index());
        if (it != activeDevices.end()) {
            if (auto existing = it->second.lock()) {
                device = existing;
            } else {
                activeDevices.erase(it);
            }
        }

        if (!device) {
            device = std::make_shared<HeliosDevice>(sdk, heliosInfo->index());
            activeDevices[heliosInfo->index()] = device;
        }
    }

    if (device) {
        device->start();
    }

    return device;
}

void HeliosManager::closeAll() {
    std::unordered_map<unsigned int, std::shared_ptr<HeliosDevice>> snapshot;
    {
        std::lock_guard lock(activeMutex);
        for (auto& [index, weak] : activeDevices) {
            if (auto dev = weak.lock()) {
                snapshot.emplace(index, std::move(dev));
            }
        }
        activeDevices.clear();
    }

    for (auto& [index, dev] : snapshot) {
        if (!dev) continue;
        dev->stop();
        dev->close();
    }

    if (sdk) {
        sdk->CloseDevices();
    }

    opened = false;
    deviceCount = 0;
}

} // namespace libera::helios
