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
    controllerCount = count > 0 ? static_cast<std::size_t>(count) : 0;
}

std::size_t IdnManager::refreshControllerCount(bool allowRescan) {
    openIfNeeded();
    if (!sdk) {
        controllerCount = 0;
        return controllerCount;
    }

    if (!allowRescan) {
        return controllerCount;
    }

    const int count = sdk->ReScanDevicesOnlyNetwork();
    if (count > 0) {
        controllerCount = static_cast<std::size_t>(count);
    }
    return controllerCount;
}

std::vector<std::unique_ptr<core::DacInfo>> IdnManager::discover() {
    std::vector<std::unique_ptr<core::DacInfo>> results;

    bool hasActive = false;
    {
        std::lock_guard lock(activeMutex);
        for (auto it = activeControllers.begin(); it != activeControllers.end();) {
            if (it->second.expired()) {
                it = activeControllers.erase(it);
            } else {
                hasActive = true;
                ++it;
            }
        }
    }

    const auto count = refreshControllerCount(!hasActive);
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
        std::optional<core::DacInfo::NetworkInfo> networkInfo;
        static constexpr const char* idnIpPrefix = "IDN: ";
        if (label.rfind(idnIpPrefix, 0) == 0 && label.size() > 5) {
            networkInfo = core::DacInfo::NetworkInfo{
                label.substr(5),
                static_cast<std::uint16_t>(IDN_PORT)};
        }
        std::string id = "idn-" + std::to_string(index);

        results.emplace_back(std::make_unique<IdnControllerInfo>(
            std::move(id),
            std::move(label),
            HELIOS_MAX_PPS_IDN,
            index,
            firmware,
            std::move(networkInfo)));
    }

    return results;
}

std::shared_ptr<core::LaserController>
IdnManager::getAndConnectToDac(const core::DacInfo& info) {
    const auto* idnInfo = dynamic_cast<const IdnControllerInfo*>(&info);
    if (!idnInfo) {
        return nullptr;
    }

    std::shared_ptr<IdnController> controller;
    {
        std::lock_guard lock(activeMutex);
        auto it = activeControllers.find(idnInfo->index());
        if (it != activeControllers.end()) {
            if (auto existing = it->second.lock()) {
                controller = existing;
            } else {
                activeControllers.erase(it);
            }
        }

        if (!controller) {
            controller = std::make_shared<IdnController>(sdk, idnInfo->index());
            activeControllers[idnInfo->index()] = controller;
        }
    }

    if (controller) {
        controller->start();
    }

    return controller;
}

void IdnManager::closeAll() {
    std::unordered_map<unsigned int, std::shared_ptr<IdnController>> snapshot;
    {
        std::lock_guard lock(activeMutex);
        for (auto& [index, weak] : activeControllers) {
            if (auto dev = weak.lock()) {
                snapshot.emplace(index, std::move(dev));
            }
        }
        activeControllers.clear();
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
    controllerCount = 0;
}

} // namespace libera::idn
