#include "libera/idn/IdnManager.hpp"

#include <algorithm>

namespace libera::idn {
namespace {

std::string makeFallbackLabel(unsigned int index) {
    return "IDN " + std::to_string(index);
}

} // namespace

IdnManager::IdnManager() {
    sdk = std::make_shared<HeliosDac>();
}

IdnManager::~IdnManager() {
    closeAll();
}

void IdnManager::openIfNeeded() {
    if (!sdk) {
        sdk = std::make_shared<HeliosDac>();
    }

    if (opened) {
        return;
    }

    const int count = sdk->OpenDevicesOnlyNetwork();
    opened = true;
    deviceCount = count > 0 ? static_cast<std::size_t>(count) : 0;
}

std::size_t IdnManager::refreshDeviceCount(bool allowRescan) {
    openIfNeeded();
    if (!sdk) {
        deviceCount = 0;
        return deviceCount;
    }

    if (!allowRescan) {
        return deviceCount;
    }

    const int count = sdk->ReScanDevicesOnlyNetwork();
    if (count > 0) {
        deviceCount = static_cast<std::size_t>(count);
    }
    return deviceCount;
}

std::vector<std::unique_ptr<core::DacInfo>> IdnManager::discover() {
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

        const int isUsb = sdk->GetIsUsb(index);
        if (isUsb != 0) {
            continue;
        }

        char name[32] = {};
        std::string label;
        if (sdk->GetName(index, name) == HELIOS_SUCCESS) {
            label = name;
        } else {
            label = makeFallbackLabel(index);
        }

        const int firmware = sdk->GetFirmwareVersion(index);
        std::string id = "idn-" + std::to_string(index);

        results.emplace_back(std::make_unique<IdnDeviceInfo>(
            std::move(id),
            std::move(label),
            HELIOS_MAX_PPS_IDN,
            index,
            firmware));
    }

    return results;
}

std::shared_ptr<core::LaserDevice>
IdnManager::getAndConnectToDac(const core::DacInfo& info) {
    const auto* idnInfo = dynamic_cast<const IdnDeviceInfo*>(&info);
    if (!idnInfo) {
        return nullptr;
    }

    std::shared_ptr<helios::HeliosDevice> device;
    {
        std::lock_guard lock(activeMutex);
        auto it = activeDevices.find(idnInfo->index());
        if (it != activeDevices.end()) {
            if (auto existing = it->second.lock()) {
                device = existing;
            } else {
                activeDevices.erase(it);
            }
        }

        if (!device) {
            device = std::make_shared<helios::HeliosDevice>(sdk, idnInfo->index());
            activeDevices[idnInfo->index()] = device;
        }
    }

    if (device) {
        device->start();
    }

    return device;
}

void IdnManager::closeAll() {
    std::unordered_map<unsigned int, std::shared_ptr<helios::HeliosDevice>> snapshot;
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

} // namespace libera::idn
