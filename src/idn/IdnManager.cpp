#include "libera/idn/IdnManager.hpp"

#include "libera/core/ActiveControllerMap.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace libera::idn {
namespace {

std::string makeFallbackLabel(unsigned int index) {
    return "IDN " + std::to_string(index);
}

constexpr std::size_t sdkNameMaxLength = 31;

std::string truncateToSdkNameLength(std::string value) {
    if (value.size() > sdkNameMaxLength) {
        value.resize(sdkNameMaxLength);
    }
    return value;
}

std::string makeServiceLabel(const IDNSL_SERVER_INFO& serverInfo,
                             const IDNSL_SERVICE_INFO& serviceInfo) {
    return truncateToSdkNameLength(
        std::string(serverInfo.hostName).append(" - ").append(serviceInfo.serviceName));
}

std::string ipv4ToString(const in_addr& addr) {
    const std::uint32_t hostOrder = ntohl(addr.s_addr);
    return std::to_string((hostOrder >> 24) & 0xFFu) + "." +
           std::to_string((hostOrder >> 16) & 0xFFu) + "." +
           std::to_string((hostOrder >> 8) & 0xFFu) + "." +
           std::to_string(hostOrder & 0xFFu);
}

std::unordered_map<std::string, core::DacInfo::NetworkInfo> discoverIdnNetworkInfoByLabel() {
    std::unordered_map<std::string, core::DacInfo::NetworkInfo> infoByLabel;
    std::unordered_set<std::string> ambiguousLabels;

    IDNSL_SERVER_INFO* firstServerInfo = nullptr;
    constexpr unsigned discoveryTimeoutMs = 600;
    const int rc = getIDNServerList(&firstServerInfo, 0, discoveryTimeoutMs);
    if (rc != 0 || !firstServerInfo) {
        if (firstServerInfo) {
            freeIDNServerList(firstServerInfo);
        }
        return infoByLabel;
    }

    for (auto* serverInfo = firstServerInfo; serverInfo != nullptr; serverInfo = serverInfo->next) {
        std::optional<core::DacInfo::NetworkInfo> endpoint;
        for (unsigned int i = 0; i < serverInfo->addressCount; ++i) {
            const auto& addressInfo = serverInfo->addressTable[i];
            if (addressInfo.errorFlags != 0) {
                continue;
            }
            endpoint = core::DacInfo::NetworkInfo{
                ipv4ToString(addressInfo.addr),
                static_cast<std::uint16_t>(IDN_PORT)};
            break;
        }
        if (!endpoint) {
            continue;
        }

        for (unsigned int i = 0; i < serverInfo->serviceCount; ++i) {
            const auto label = makeServiceLabel(*serverInfo, serverInfo->serviceTable[i]);
            const auto [it, inserted] = infoByLabel.emplace(label, *endpoint);
            if (!inserted &&
                (it->second.ip != endpoint->ip || it->second.port != endpoint->port)) {
                ambiguousLabels.insert(label);
            }
        }
    }

    freeIDNServerList(firstServerInfo);

    for (const auto& label : ambiguousLabels) {
        infoByLabel.erase(label);
    }

    return infoByLabel;
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
        hasActive = core::pruneExpiredActiveControllers(activeControllers);
    }

    // Re-scan only when there are no live controllers. This keeps stable SDK
    // indices while an IDN controller object is in use.
    const auto count = refreshControllerCount(!hasActive);
    if (!sdk || count == 0) {
        return results;
    }

    const auto networkInfoByLabel = discoverIdnNetworkInfoByLabel();

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
        if (const auto it = networkInfoByLabel.find(truncateToSdkNameLength(label));
            it != networkInfoByLabel.end()) {
            networkInfo = it->second;
        }
        static constexpr const char* idnIpPrefix = "IDN: ";
        if (!networkInfo && label.rfind(idnIpPrefix, 0) == 0 && label.size() > 5) {
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
        controller = core::getOrCreateActiveController(
            activeControllers,
            idnInfo->index(),
            [this, idnInfo] { return std::make_shared<IdnController>(sdk, idnInfo->index()); });
    }

    if (controller) {
        // Keep existing behavior: calling getAndConnectToDac can re-start a controller.
        controller->start();
    }

    return controller;
}

void IdnManager::closeAll() {
    std::unordered_map<unsigned int, std::shared_ptr<IdnController>> snapshot;
    {
        std::lock_guard lock(activeMutex);
        snapshot = core::snapshotActiveControllersAndClear(activeControllers);
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
