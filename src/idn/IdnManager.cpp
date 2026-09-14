#include "libera/idn/IdnManager.hpp"

#include "libera/log/Log.hpp"

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
    std::string controllerId;
    core::ControllerInfo::NetworkInfo networkInfo;
    unsigned int serviceId = 0;
    std::string hostName;
    std::string serviceName;
    std::string displayLabel;
};

// Snapshot of one raw discovery pass. The same service can be looked up either by
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

std::string makeFallbackControllerId(unsigned int index) {
    return "idn-" + makeFallbackUnitId(index);
}

std::string makeControllerIdFromServiceId(const std::string& unitId,
                                          unsigned int serviceId) {
    return "idn-" + unitId + "-service-" + std::to_string(serviceId);
}

constexpr std::size_t sdkNameMaxLength = 31;

std::string truncateToSdkNameLength(std::string value) {
    if (value.size() > sdkNameMaxLength) {
        value.resize(sdkNameMaxLength);
    }
    return value;
}

std::string makeSdkServiceLabel(const IDNSL_SERVER_INFO& serverInfo,
                                const IDNSL_SERVICE_INFO& serviceInfo) {
    return truncateToSdkNameLength(
        std::string(serverInfo.hostName).append(" - ").append(serviceInfo.serviceName));
}

// IDN hosts can expose several services, and service names are often generic
// ("Main"). Show both parts so the user sees the named hardware plus output.
std::string makeDisplayServiceLabel(const IDNSL_SERVER_INFO& serverInfo,
                                    const IDNSL_SERVICE_INFO& serviceInfo,
                                    const std::string& fallbackLabel) {
    const std::string hostName = serverInfo.hostName;
    const std::string serviceName = serviceInfo.serviceName;
    if (!hostName.empty() && !serviceName.empty()) {
        return hostName + " - " + serviceName;
    }
    if (!hostName.empty()) {
        return hostName;
    }
    if (!serviceName.empty()) {
        return serviceName;
    }
    return fallbackLabel;
}

IdnController::HealthEndpoint makeHealthEndpoint(
    const core::ControllerInfo::NetworkInfo& networkInfo) {
    return IdnController::HealthEndpoint{networkInfo.ip, networkInfo.port};
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
    // readable and stable string so it can safely be used inside controller keys.
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
            const auto& serviceInfo = serverInfo->serviceTable[i];
            const auto sdkLabel = makeSdkServiceLabel(*serverInfo, serviceInfo);
            DiscoveredIdnService service{
                unitId,
                makeControllerIdFromServiceId(unitId, serviceInfo.serviceID),
                *endpoint,
                serviceInfo.serviceID,
                serverInfo->hostName,
                serviceInfo.serviceName,
                makeDisplayServiceLabel(*serverInfo, serviceInfo, sdkLabel),
            };
            snapshot.servicesByLabel[sdkLabel].push_back(service);
            snapshot.servicesByIp[endpoint->ip].push_back(std::move(service));
        }
    }

    freeIDNServerList(firstServerInfo);
    return snapshot;
}

std::size_t countDiscoveredServices(const IdnDiscoverySnapshot& snapshot) {
    std::size_t count = 0;
    for (const auto& [label, services] : snapshot.servicesByLabel) {
        (void)label;
        count += services.size();
    }
    return count;
}

std::optional<DiscoveredIdnService> matchDiscoveredService(
    const std::unordered_map<std::string, std::vector<DiscoveredIdnService>>& servicesByKey,
    const std::string& key,
    const std::string* preferredControllerId,
    const std::unordered_set<std::string>& usedControllerIds) {
    const auto servicesIt = servicesByKey.find(key);
    if (servicesIt == servicesByKey.end()) {
        return std::nullopt;
    }

    const auto& services = servicesIt->second;
    if (preferredControllerId) {
        // First try to keep the same service bound to the same SDK slot as the
        // previous snapshot. That preserves controller identity even if several
        // services share the same display label or unit ID.
        for (const auto& service : services) {
            if (service.controllerId == *preferredControllerId &&
                usedControllerIds.find(service.controllerId) == usedControllerIds.end()) {
                return service;
            }
        }
    }

    // Otherwise pick the first still-unclaimed service for this key. The
    // caller tracks which service-level controller IDs were already assigned
    // during this snapshot.
    for (const auto& service : services) {
        if (usedControllerIds.find(service.controllerId) == usedControllerIds.end()) {
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
    for (const auto& [controllerId, controller] : activeSnapshot) {
        (void)controllerId;
        if (controller && !controller->isConnected()) {
            hasDisconnectedActive = true;
            break;
        }
    }

    // Re-scan policy:
    // keep SDK indices stable during steady-state playback, but allow a rescan
    // once an active controller has already dropped. That gives the manager a
    // chance to reopen IDN devices and remap the stable service ID to whichever
    // transient SDK index it came back on.
    const auto count = refreshControllerCount(!hasActive || hasDisconnectedActive);
    if (!sdk || count == 0) {
        if (emptySdkDiagnosticCountdown == 0) {
            const auto rawDiscoverySnapshot = discoverIdnServices();
            const auto rawServiceCount = countDiscoveredServices(rawDiscoverySnapshot);
            if (rawServiceCount > 0) {
                logError("[IdnManager] raw IDN discovery found",
                         rawServiceCount,
                         "service(s), but the Helios SDK returned no network devices");
            }
            emptySdkDiagnosticCountdown = 5;
        } else {
            --emptySdkDiagnosticCountdown;
        }
        return results;
    }

    emptySdkDiagnosticCountdown = 0;
    const auto discoverySnapshot = discoverIdnServices();
    std::unordered_set<std::string> usedControllerIds;
    std::unordered_set<unsigned int> seenIndices;
    std::size_t closedSlotCount = 0;
    std::size_t usbSlotCount = 0;

    results.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
        const int closed = sdk->GetIsClosed(index);
        if (closed > 0) {
            ++closedSlotCount;
            continue;
        }

        const int isUsb = sdk->GetIsUsb(index);
        if (isUsb != 0) {
            ++usbSlotCount;
            continue;
        }

        seenIndices.insert(index);

        char name[32] = {};
        std::string sdkLabel;
        if (sdk->GetName(index, name) == HELIOS_SUCCESS) {
            sdkLabel = name;
        } else {
            sdkLabel = makeFallbackLabel(index);
        }
        std::string label = sdkLabel;

        const int firmware = sdk->GetFirmwareVersion(index);
        const auto stableUnitIdIt = stableUnitIdByIndex.find(index);
        const auto stableControllerIdIt = stableControllerIdByIndex.find(index);
        const std::string* preferredControllerId =
            stableControllerIdIt != stableControllerIdByIndex.end() ?
                &stableControllerIdIt->second :
                nullptr;

        // Strategy:
        // derive a stable service ID from raw IDN discovery, while still using the
        // SDK index as the transient runtime handle for this specific snapshot.
        std::optional<DiscoveredIdnService> matchedService =
            matchDiscoveredService(discoverySnapshot.servicesByLabel,
                                   truncateToSdkNameLength(sdkLabel),
                                   preferredControllerId,
                                   usedControllerIds);

        std::optional<core::ControllerInfo::NetworkInfo> networkInfo;
        static constexpr const char* idnIpPrefix = "IDN: ";
        if (!matchedService && sdkLabel.rfind(idnIpPrefix, 0) == 0 && sdkLabel.size() > 5) {
            // Some SDK fallback names only expose the IP address. Use that as a
            // second lookup path so we can still recover the stable service ID.
            matchedService = matchDiscoveredService(discoverySnapshot.servicesByIp,
                                                    sdkLabel.substr(5),
                                                    preferredControllerId,
                                                    usedControllerIds);
        }

        std::string unitId;
        std::string controllerId;
        unsigned int serviceId = 0;
        std::string hostName;
        std::string serviceName;
        if (matchedService) {
            unitId = matchedService->unitId;
            controllerId = matchedService->controllerId;
            serviceId = matchedService->serviceId;
            networkInfo = matchedService->networkInfo;
            hostName = matchedService->hostName;
            serviceName = matchedService->serviceName;
            if (!matchedService->displayLabel.empty()) {
                label = matchedService->displayLabel;
            }
        } else if (stableUnitIdIt != stableUnitIdByIndex.end() &&
                   stableControllerIdIt != stableControllerIdByIndex.end()) {
            unitId = stableUnitIdIt->second;
            controllerId = stableControllerIdIt->second;
        } else {
            unitId = makeFallbackUnitId(index);
            controllerId = makeFallbackControllerId(index);
        }

        if (usedControllerIds.find(controllerId) != usedControllerIds.end()) {
            // A duplicate here means the current snapshot could not map two SDK
            // slots back to distinct protocol services. Keep them separate for
            // now so we do not accidentally collapse live controllers together.
            unitId = makeFallbackUnitId(index);
            controllerId = makeFallbackControllerId(index);
            serviceId = 0;
        }
        usedControllerIds.insert(controllerId);
        stableUnitIdByIndex[index] = unitId;
        stableControllerIdByIndex[index] = controllerId;

        const auto activeIt = activeSnapshot.find(controllerId);
        if (activeIt != activeSnapshot.end() && activeIt->second) {
            if (activeIt->second->controllerIndex() != index) {
                activeIt->second->updateControllerIndex(index);
            }
            if (networkInfo) {
                activeIt->second->setHealthEndpoint(makeHealthEndpoint(*networkInfo));
            }
            if (matchedService) {
                activeIt->second->noteDiscoverySeen();
            }
        }

        if (!networkInfo && sdkLabel.rfind(idnIpPrefix, 0) == 0 && sdkLabel.size() > 5) {
            networkInfo = core::ControllerInfo::NetworkInfo{
                sdkLabel.substr(5),
                static_cast<std::uint16_t>(IDN_PORT)};
        }

        results.emplace_back(std::make_unique<IdnControllerInfo>(
            std::move(controllerId),
            std::move(unitId),
            serviceId,
            std::move(label),
            HELIOS_MAX_PPS_IDN,
            index,
            firmware,
            std::move(networkInfo),
            std::move(hostName),
            std::move(serviceName)));
    }

    if (results.empty()) {
        if (!reportedSdkSlotsWithoutResults) {
            logInfo("[IdnManager] SDK reported",
                    count,
                    "network slot(s), but none produced a usable IDN controller",
                    "closed",
                    closedSlotCount,
                    "usb",
                    usbSlotCount);
            reportedSdkSlotsWithoutResults = true;
        }
    } else {
        reportedSdkSlotsWithoutResults = false;
    }

    for (auto it = stableUnitIdByIndex.begin(); it != stableUnitIdByIndex.end();) {
        // Drop cached slot-to-service bindings for SDK indices that no longer
        // exist in the latest snapshot. Fresh discoveries will rebuild them.
        if (seenIndices.find(it->first) == seenIndices.end()) {
            it = stableUnitIdByIndex.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = stableControllerIdByIndex.begin(); it != stableControllerIdByIndex.end();) {
        if (seenIndices.find(it->first) == seenIndices.end()) {
            it = stableControllerIdByIndex.erase(it);
        } else {
            ++it;
        }
    }

    return results;
}

std::string
IdnManager::controllerKey(const IdnControllerInfo& info) const {
    return info.idValue();
}

std::shared_ptr<IdnController>
IdnManager::createController(const IdnControllerInfo& info) {
    IdnController::HealthEndpoint endpoint;
    if (const auto& networkInfo = info.networkInfo()) {
        endpoint = makeHealthEndpoint(*networkInfo);
    }
    return std::make_shared<IdnController>(sdk, info.index(), std::move(endpoint));
}

IdnManager::NewControllerDisposition
IdnManager::prepareNewController(IdnController& controller,
                                 const IdnControllerInfo& info) {
    if (const auto& networkInfo = info.networkInfo()) {
        controller.setHealthEndpoint(makeHealthEndpoint(*networkInfo));
        controller.noteDiscoverySeen();
    }
    controller.startHealthMonitoring();
    // Keep existing behavior: calling connectController can re-start a controller.
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void IdnManager::prepareExistingController(IdnController& controller,
                                           const IdnControllerInfo& info) {
    if (const auto& networkInfo = info.networkInfo()) {
        controller.setHealthEndpoint(makeHealthEndpoint(*networkInfo));
        controller.noteDiscoverySeen();
    }
    controller.startHealthMonitoring();
    controller.startThread();
}

void IdnManager::closeController(const std::string& key,
                                 IdnController& controller) {
    (void)key;
    controller.stopHealthMonitoring();
    controller.close();
}

void IdnManager::afterCloseControllers() {
    if (sdk) {
        sdk->CloseDevices();
    }

    opened = false;
    controllerCount = 0;
    emptySdkDiagnosticCountdown = 0;
    reportedSdkSlotsWithoutResults = false;
    stableUnitIdByIndex.clear();
    stableControllerIdByIndex.clear();
}

} // namespace libera::idn
