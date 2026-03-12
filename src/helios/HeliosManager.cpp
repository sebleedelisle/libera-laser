#include "libera/helios/HeliosManager.hpp"

#include "libera/core/ActiveControllerMap.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace libera::helios {
namespace {

std::string makeFallbackLabel(unsigned int index) {
    return "Helios " + std::to_string(index);
}

bool isSdkFallbackUnknownLabel(const char* label) {
    if (label == nullptr || label[0] == '\0') {
        return false;
    }

    // The bundled Helios SDK synthesizes "Unknown Helios NN" and still returns
    // HELIOS_SUCCESS when GetName() control traffic fails. Treat that label as
    // non-authoritative so we do not churn stable IDs during transient USB errors.
    constexpr const char kUnknownPrefix[] = "Unknown Helios ";
    constexpr std::size_t kPrefixLen = sizeof(kUnknownPrefix) - 1;
    return std::strncmp(label, kUnknownPrefix, kPrefixLen) == 0;
}

std::string sanitizeIdComponent(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else if (ch == ' ' || ch == '-' || ch == '_' || ch == '.') {
            out.push_back('-');
        }
    }

    // Collapse repeated separators.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool previousDash = false;
    for (char ch : out) {
        if (ch == '-') {
            if (!previousDash) {
                collapsed.push_back(ch);
            }
            previousDash = true;
        } else {
            collapsed.push_back(ch);
            previousDash = false;
        }
    }

    // Trim leading/trailing separator.
    while (!collapsed.empty() && collapsed.front() == '-') {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && collapsed.back() == '-') {
        collapsed.pop_back();
    }
    return collapsed;
}

std::string makeHeliosControllerId(const std::string& label,
                                   unsigned int index,
                                   std::unordered_set<std::string>& usedIds) {
    std::string base = sanitizeIdComponent(label);
    if (base.empty()) {
        base = "helios";
    }

    std::string candidate = base;
    if (usedIds.find(candidate) != usedIds.end()) {
        candidate = base + "-" + std::to_string(index);
    }
    int suffix = 2;
    while (usedIds.find(candidate) != usedIds.end()) {
        candidate = base + "-" + std::to_string(suffix++);
    }
    usedIds.insert(candidate);
    return candidate;
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
    controllerCount = count > 0 ? static_cast<std::size_t>(count) : 0;
}

std::size_t HeliosManager::refreshControllerCount(bool allowRescan) {
    openIfNeeded();
    if (!sdk) {
        controllerCount = 0;
        return controllerCount;
    }

    if (!allowRescan) {
        return controllerCount;
    }

    const int count = sdk->ReScanDevicesOnlyUsb();
    if (count > 0) {
        controllerCount = static_cast<std::size_t>(count);
    }
    return controllerCount;
}

std::vector<std::unique_ptr<core::DacInfo>> HeliosManager::discover() {
    std::vector<std::unique_ptr<core::DacInfo>> results;

    bool hasActive = false;
    {
        std::lock_guard lock(activeMutex);
        hasActive = core::pruneExpiredActiveControllers(activeControllers);
    }

    // Re-scan only when there are no live controllers. This avoids resetting
    // SDK device indices while a controller instance is active.
    const auto count = refreshControllerCount(!hasActive);
    if (!sdk || count == 0) {
        return results;
    }

    results.reserve(count);
    std::unordered_set<std::string> usedIds;
    std::unordered_set<unsigned int> seenIndices;
    for (unsigned int index = 0; index < count; ++index) {
        const int closed = sdk->GetIsClosed(index);
        if (closed > 0) {
            continue;
        }
        seenIndices.insert(index);

        char name[32] = {};
        std::string label;
        // Strategy:
        // while an active controller is streaming, avoid repeated GetName()
        // control transfers every discovery tick. We only probe name eagerly
        // when no active stream exists or we have no cached label yet.
        bool attemptedFreshNameRead = false;
        const bool needsNameRead =
            (!hasActive) || (stableLabelByIndex.find(index) == stableLabelByIndex.end());
        if (needsNameRead) {
            attemptedFreshNameRead = true;
            if (sdk->GetName(index, name) == HELIOS_SUCCESS &&
                name[0] != '\0' &&
                !isSdkFallbackUnknownLabel(name)) {
                label = name;
                stableLabelByIndex[index] = label;
            }
        }

        if (label.empty()) {
            auto cachedLabel = stableLabelByIndex.find(index);
            if (cachedLabel != stableLabelByIndex.end()) {
                label = cachedLabel->second;
            }
        }

        if (label.empty()) {
            label = makeFallbackLabel(index);
            if (attemptedFreshNameRead) {
                logInfo("[HeliosManager] falling back to synthetic label",
                        "index", index,
                        "label", label,
                        "sdk_name", name[0] == '\0' ? "<empty>" : std::string(name));
            }
        } else {
            stableLabelByIndex[index] = label;
        }

        const int isUsb = sdk->GetIsUsb(index);
        const bool usbController = isUsb == 1;
        const std::uint32_t maxRate = usbController ? HELIOS_MAX_PPS : HELIOS_MAX_PPS_IDN;
        const int firmware = sdk->GetFirmwareVersion(index);
        std::string id;
        auto cachedId = stableIdByIndex.find(index);
        if (cachedId != stableIdByIndex.end()) {
            id = cachedId->second;
            if (usedIds.find(id) == usedIds.end()) {
                usedIds.insert(id);
            } else {
                // Very defensive: if a duplicate sneaks in, regenerate once.
                id = makeHeliosControllerId(label, index, usedIds);
                stableIdByIndex[index] = id;
            }
        } else {
            id = makeHeliosControllerId(label, index, usedIds);
            stableIdByIndex[index] = id;
        }

        results.emplace_back(std::make_unique<HeliosControllerInfo>(
            std::move(id),
            std::move(label),
            maxRate,
            index,
            usbController,
            firmware));
    }

    // Drop stale cache entries for devices no longer present in current scan.
    for (auto it = stableIdByIndex.begin(); it != stableIdByIndex.end();) {
        if (seenIndices.find(it->first) == seenIndices.end()) {
            it = stableIdByIndex.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = stableLabelByIndex.begin(); it != stableLabelByIndex.end();) {
        if (seenIndices.find(it->first) == seenIndices.end()) {
            it = stableLabelByIndex.erase(it);
        } else {
            ++it;
        }
    }

    return results;
}

std::shared_ptr<core::LaserController>
HeliosManager::getAndConnectToDac(const core::DacInfo& info) {
    const auto* heliosInfo = dynamic_cast<const HeliosControllerInfo*>(&info);
    if (!heliosInfo) {
        return nullptr;
    }

    std::shared_ptr<HeliosController> controller;
    {
        std::lock_guard lock(activeMutex);
        controller = core::getOrCreateActiveController(
            activeControllers,
            heliosInfo->index(),
            [this, heliosInfo] { return std::make_shared<HeliosController>(sdk, heliosInfo->index()); });
    }

    if (controller) {
        // Keep existing behavior: calling getAndConnectToDac can re-start a controller.
        controller->start();
    }

    return controller;
}

void HeliosManager::closeAll() {
    std::unordered_map<unsigned int, std::shared_ptr<HeliosController>> snapshot;
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
    stableIdByIndex.clear();
    stableLabelByIndex.clear();
}

} // namespace libera::helios
