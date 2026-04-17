#include "libera/idn/IdnManager.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace libera::idn {
namespace {

// Minimal view of one service announced by the raw IDN discovery layer.
// We keep the immutable unit ID together with the currently reachable endpoint
// so the manager can build stable controller identities without depending on
// the Helios SDK's transient device indices.
struct DiscoveredIdnService {
    std::string unitId;
    core::ControllerInfo::NetworkInfo networkInfo;
};

// Snapshot of one raw discovery pass. The same unit can be looked up either by
// its service label or, for SDK fallback names, by its advertised IP address.
struct IdnDiscoverySnapshot {
    std::unordered_map<std::string, std::vector<DiscoveredIdnService>> servicesByLabel;
    std::unordered_map<std::string, std::vector<DiscoveredIdnService>> servicesByIp;
};

std::string makeFallbackLabel(unsigned int index) {
    return "IDN " + std::to_string(index);
}

std::string makeFallbackUnitId(unsigned int index) {
    return "unknown-index-" + std::to_string(index);
}

std::string makeControllerIdFromUnitId(const std::string& unitId) {
    return "idn-" + unitId;
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

std::string encodeUnitIdHex(const std::uint8_t* unitId) {
    // The IDN hello packet gives us a binary 16-byte unit ID. Convert it to a
    // readable and stable string so it can safely be used as a controller key.
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(IDNSL_UNITID_LENGTH * 2);
    for (unsigned int i = 0; i < IDNSL_UNITID_LENGTH; ++i) {
        const unsigned char byte = unitId[i];
        hex.push_back(hexDigits[(byte >> 4) & 0x0F]);
        hex.push_back(hexDigits[byte & 0x0F]);
    }
    return hex;
}

IdnDiscoverySnapshot discoverIdnServices() {
    // Perform a raw IDN hello scan independent of the Helios SDK device list.
    // This gives us the protocol-level unit IDs that stay stable across SDK
    // rescans, reconnects, and device index reshuffles.
    IdnDiscoverySnapshot snapshot;

    IDNSL_SERVER_INFO* firstServerInfo = nullptr;
    constexpr unsigned discoveryTimeoutMs = 600;
    const int rc = getIDNServerList(&firstServerInfo, 0, discoveryTimeoutMs);
    if (rc != 0 || !firstServerInfo) {
        if (firstServerInfo) {
            freeIDNServerList(firstServerInfo);
        }
        return snapshot;
    }

    for (auto* serverInfo = firstServerInfo; serverInfo != nullptr; serverInfo = serverInfo->next) {
        std::optional<core::ControllerInfo::NetworkInfo> endpoint;
        for (unsigned int i = 0; i < serverInfo->addressCount; ++i) {
            const auto& addressInfo = serverInfo->addressTable[i];
            if (addressInfo.errorFlags != 0) {
                continue;
            }
            endpoint = core::ControllerInfo::NetworkInfo{
                ipv4ToString(addressInfo.addr),
                static_cast<std::uint16_t>(IDN_PORT)};
            break;
        }
        if (!endpoint) {
            continue;
        }

        const std::string unitId = encodeUnitIdHex(serverInfo->unitID);
        for (unsigned int i = 0; i < serverInfo->serviceCount; ++i) {
            const auto label = makeServiceLabel(*serverInfo, serverInfo->serviceTable[i]);
            DiscoveredIdnService service{unitId, *endpoint};
            snapshot.servicesByLabel[label].push_back(service);
            snapshot.servicesByIp[endpoint->ip].push_back(std::move(service));
        }
    }

    freeIDNServerList(firstServerInfo);
    return snapshot;
}

std::optional<DiscoveredIdnService> matchDiscoveredService(
    const std::unordered_map<std::string, std::vector<DiscoveredIdnService>>& servicesByKey,
    const std::string& key,
    const std::string* preferredUnitId,
    const std::unordered_set<std::string>& usedUnitIds) {
    const auto servicesIt = servicesByKey.find(key);
    if (servicesIt == servicesByKey.end()) {
        return std::nullopt;
    }

    const auto& services = servicesIt->second;
    if (preferredUnitId) {
        // First try to keep the same unit ID bound to the same SDK slot as the
        // previous snapshot. That preserves controller identity even if several
        // services share the same display label.
        for (const auto& service : services) {
            if (service.unitId == *preferredUnitId &&
                usedUnitIds.find(service.unitId) == usedUnitIds.end()) {
                return service;
            }
        }
    }

    // Otherwise pick the first still-unclaimed service for this key. The
    // caller tracks which unit IDs were already assigned during this snapshot.
    for (const auto& service : services) {
        if (usedUnitIds.find(service.unitId) == usedUnitIds.end()) {
            return service;
        }
    }

    return std::nullopt;
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

std::vector<std::unique_ptr<core::ControllerInfo>> IdnManager::discover() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;

    const auto activeSnapshot = liveControllers();
    const bool hasActive = !activeSnapshot.empty();
    bool hasDisconnectedActive = false;
    for (const auto& [unitId, controller] : activeSnapshot) {
        (void)unitId;
        if (controller && !controller->isConnected()) {
            hasDisconnectedActive = true;
            break;
        }
    }

    // Re-scan policy:
    // keep SDK indices stable during steady-state playback, but allow a rescan
    // once an active controller has already dropped. That gives the manager a
    // chance to reopen IDN devices and remap the stable unit ID to whichever
    // transient SDK index it came back on.
    const auto count = refreshControllerCount(!hasActive || hasDisconnectedActive);
    if (!sdk || count == 0) {
        return results;
    }

    const auto discoverySnapshot = discoverIdnServices();
    std::unordered_set<std::string> usedUnitIds;
    std::unordered_set<unsigned int> seenIndices;

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

        seenIndices.insert(index);

        char name[32] = {};
        std::string label;
        if (sdk->GetName(index, name) == HELIOS_SUCCESS) {
            label = name;
        } else {
            label = makeFallbackLabel(index);
        }

        const int firmware = sdk->GetFirmwareVersion(index);
        const auto stableUnitIdIt = stableUnitIdByIndex.find(index);
        const std::string* preferredUnitId =
            stableUnitIdIt != stableUnitIdByIndex.end() ? &stableUnitIdIt->second : nullptr;

        // Strategy:
        // derive a stable unit ID from raw IDN discovery, while still using the
        // SDK index as the transient runtime handle for this specific snapshot.
        std::optional<DiscoveredIdnService> matchedService =
            matchDiscoveredService(discoverySnapshot.servicesByLabel,
                                   truncateToSdkNameLength(label),
                                   preferredUnitId,
                                   usedUnitIds);

        std::optional<core::ControllerInfo::NetworkInfo> networkInfo;
        static constexpr const char* idnIpPrefix = "IDN: ";
        if (!matchedService && label.rfind(idnIpPrefix, 0) == 0 && label.size() > 5) {
            // Some SDK fallback names only expose the IP address. Use that as a
            // second lookup path so we can still recover the stable unit ID.
            matchedService = matchDiscoveredService(discoverySnapshot.servicesByIp,
                                                    label.substr(5),
                                                    preferredUnitId,
                                                    usedUnitIds);
        }

        std::string unitId;
        if (matchedService) {
            unitId = matchedService->unitId;
            networkInfo = matchedService->networkInfo;
        } else if (stableUnitIdIt != stableUnitIdByIndex.end()) {
            unitId = stableUnitIdIt->second;
        } else {
            unitId = makeFallbackUnitId(index);
        }

        if (usedUnitIds.find(unitId) != usedUnitIds.end()) {
            // A duplicate here means the current snapshot could not map two SDK
            // slots back to distinct protocol identities. Keep them separate for
            // now so we do not accidentally collapse live controllers together.
            unitId = makeFallbackUnitId(index);
        }
        usedUnitIds.insert(unitId);
        stableUnitIdByIndex[index] = unitId;

        const auto activeIt = activeSnapshot.find(unitId);
        if (activeIt != activeSnapshot.end() && activeIt->second) {
            if (activeIt->second->controllerIndex() != index) {
                activeIt->second->updateControllerIndex(index);
            }
        }

        if (!networkInfo && label.rfind(idnIpPrefix, 0) == 0 && label.size() > 5) {
            networkInfo = core::ControllerInfo::NetworkInfo{
                label.substr(5),
                static_cast<std::uint16_t>(IDN_PORT)};
        }
        std::string id = makeControllerIdFromUnitId(unitId);

        results.emplace_back(std::make_unique<IdnControllerInfo>(
            std::move(id),
            std::move(unitId),
            std::move(label),
            HELIOS_MAX_PPS_IDN,
            index,
            firmware,
            std::move(networkInfo)));
    }

    for (auto it = stableUnitIdByIndex.begin(); it != stableUnitIdByIndex.end();) {
        // Drop cached slot-to-unit bindings for SDK indices that no longer
        // exist in the latest snapshot. Fresh discoveries will rebuild them.
        if (seenIndices.find(it->first) == seenIndices.end()) {
            it = stableUnitIdByIndex.erase(it);
        } else {
            ++it;
        }
    }

    return results;
}

std::string
IdnManager::controllerKey(const IdnControllerInfo& info) const {
    return info.unitId();
}

std::shared_ptr<IdnController>
IdnManager::createController(const IdnControllerInfo& info) {
    return std::make_shared<IdnController>(sdk, info.index());
}

IdnManager::NewControllerDisposition
IdnManager::prepareNewController(IdnController& controller,
                                 const IdnControllerInfo& info) {
    (void)info;
    // Keep existing behavior: calling connectController can re-start a controller.
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void IdnManager::prepareExistingController(IdnController& controller,
                                           const IdnControllerInfo& info) {
    (void)info;
    controller.startThread();
}

void IdnManager::closeController(const std::string& key,
                                 IdnController& controller) {
    (void)key;
    controller.close();
}

void IdnManager::afterCloseControllers() {
    if (sdk) {
        sdk->CloseDevices();
    }

    opened = false;
    controllerCount = 0;
    stableUnitIdByIndex.clear();
}

} // namespace libera::idn
