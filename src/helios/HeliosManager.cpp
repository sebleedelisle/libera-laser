#include "libera/helios/HeliosManager.hpp"

#include "libera/core/ActiveControllerMap.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace libera::helios {
namespace {

constexpr std::size_t maxHeliosNameLength = 30;

std::string makeFallbackLabel(unsigned int index) {
    return "Helios " + std::to_string(index);
}

std::string truncateHeliosName(const std::string& label) {
    // Firmware stores up to 31 bytes including trailing null terminator.
    if (label.size() <= maxHeliosNameLength) {
        return label;
    }
    return label.substr(0, maxHeliosNameLength);
}

std::string makeUniqueRenameLabelWithSuffix(unsigned int index,
                                            const std::string& preferredBase,
                                            int suffix) {
    std::string base = truncateHeliosName(preferredBase);
    if (base.empty()) {
        base = truncateHeliosName(makeFallbackLabel(index));
    }

    const std::string suffixLabel = " (" + std::to_string(suffix) + ")";
    if (suffixLabel.size() >= maxHeliosNameLength) {
        return truncateHeliosName(base);
    }

    const std::size_t maxBaseLength = maxHeliosNameLength - suffixLabel.size();
    if (base.size() > maxBaseLength) {
        base.resize(maxBaseLength);
        while (!base.empty() && base.back() == ' ') {
            base.pop_back();
        }
        if (base.empty()) {
            base = makeFallbackLabel(index);
            if (base.size() > maxBaseLength) {
                base.resize(maxBaseLength);
            }
        }
    }

    return base + suffixLabel;
}

std::string makeUniqueRenameLabel(unsigned int index,
                                  const std::string& preferredBase,
                                  const std::string& currentLabel,
                                  const std::unordered_set<std::string>& usedLabels) {
    std::string candidate = truncateHeliosName(preferredBase);
    if (candidate.empty()) {
        candidate = truncateHeliosName(makeFallbackLabel(index));
    }

    const std::string truncatedCurrentLabel = truncateHeliosName(currentLabel);
    if (candidate != truncatedCurrentLabel &&
        usedLabels.find(candidate) == usedLabels.end()) {
        return candidate;
    }

    for (int suffix = 2;; ++suffix) {
        candidate = makeUniqueRenameLabelWithSuffix(index, preferredBase, suffix);
        if (candidate != truncatedCurrentLabel &&
            usedLabels.find(candidate) == usedLabels.end()) {
            return candidate;
        }
    }
}

bool trySetHeliosName(HeliosDac& sdk,
                      unsigned int index,
                      const std::string& newLabel) {
    std::array<char, 31> name = {};
    const std::string safeName = truncateHeliosName(newLabel);
    std::strncpy(name.data(), safeName.c_str(), name.size() - 1);
    return sdk.SetName(index, name.data()) == HELIOS_SUCCESS;
}

bool isSdkFallbackUnknownLabel(const char* label) {
    if (label == nullptr || label[0] == '\0') {
        return false;
    }

    // The bundled Helios SDK synthesizes "Unknown Helios NN" and still returns
    // HELIOS_SUCCESS when GetName() control traffic fails. Treat that label as
    // non-authoritative so we do not churn stable IDs during transient USB errors.
    constexpr const char unknownPrefix[] = "Unknown Helios ";
    constexpr std::size_t prefixLen = sizeof(unknownPrefix) - 1;
    return std::strncmp(label, unknownPrefix, prefixLen) == 0;
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

std::vector<std::unique_ptr<core::ControllerInfo>> HeliosManager::discover() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;

    bool hasActive = false;
    bool hasConnectedActive = false;
    {
        std::lock_guard lock(activeMutex);
        hasActive = core::pruneExpiredActiveControllers(activeControllers);
        if (hasActive) {
            for (const auto& [controllerName, weakController] : activeControllers) {
                (void)controllerName;
                const auto controller = weakController.lock();
                if (!controller) {
                    continue;
                }
                if (controller->isConnected()) {
                    hasConnectedActive = true;
                    break;
                }
            }
        }
    }

    // Re-scan when there are no currently connected live controllers.
    // This preserves stable USB indices while streaming but still allows
    // unplug/replug recovery for active wrappers.
    const auto count = refreshControllerCount(!hasConnectedActive);
    if (!sdk || count == 0) {
        return results;
    }

    results.reserve(count);
    std::unordered_set<std::string> usedIds;
    std::unordered_set<std::string> usedLabels;
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
        bool sdkReportedEmptyName = false;
        const bool needsNameRead =
            (!hasActive) || (stableLabelByIndex.find(index) == stableLabelByIndex.end());
        if (needsNameRead) {
            attemptedFreshNameRead = true;
            if (sdk->GetName(index, name) == HELIOS_SUCCESS) {
                if (name[0] == '\0') {
                    sdkReportedEmptyName = true;
                } else if (!isSdkFallbackUnknownLabel(name)) {
                    label = name;
                    stableLabelByIndex[index] = label;
                }
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

        // Device names should be unique for stable UX/routing. If duplicates are
        // found, or the device reports an empty name, auto-repair by persisting
        // a synthesized unique label while no active stream is running.
        const bool duplicateLabel = usedLabels.find(label) != usedLabels.end();
        const bool shouldPersistUniqueLabel = !hasActive && (duplicateLabel || sdkReportedEmptyName);
        if (shouldPersistUniqueLabel) {
            const std::string originalDeviceLabel = sdkReportedEmptyName ? std::string() : label;
            const std::string renamedLabel =
                makeUniqueRenameLabel(index, label, originalDeviceLabel, usedLabels);
            if (trySetHeliosName(*sdk, index, renamedLabel)) {
                logInfo("[HeliosManager] Helios name auto-renamed",
                        "index", index,
                        "reason", sdkReportedEmptyName ? "empty" : "duplicate",
                        "old_label", sdkReportedEmptyName ? std::string("<empty>") : label,
                        "new_label", renamedLabel);
                label = renamedLabel;
                stableLabelByIndex[index] = label;
                usedLabels.insert(label);
            } else {
                logError("[HeliosManager] failed to auto-rename Helios name",
                         "index", index,
                         "reason", sdkReportedEmptyName ? "empty" : "duplicate",
                         "label", label,
                         "requested_label", renamedLabel);
                usedLabels.insert(label);
            }
        } else {
            usedLabels.insert(label);
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
HeliosManager::connectController(const core::ControllerInfo& info) {
    const auto* heliosInfo = dynamic_cast<const HeliosControllerInfo*>(&info);
    if (!heliosInfo) {
        return nullptr;
    }

    const std::string& controllerName = heliosInfo->labelValue();
    std::shared_ptr<HeliosController> controller;
    std::shared_ptr<HeliosController> staleController;
    {
        std::lock_guard lock(activeMutex);
        auto it = activeControllers.find(controllerName);
        if (it != activeControllers.end()) {
            controller = it->second.lock();
            if (!controller) {
                activeControllers.erase(it);
            } else if (controller->controllerIndex() != heliosInfo->index()) {
                staleController = std::move(controller);
                activeControllers.erase(it);
            }
        }

        if (!controller) {
            controller = std::make_shared<HeliosController>(sdk, heliosInfo->index());
            activeControllers.insert_or_assign(controllerName, controller);
        }
    }

    if (staleController) {
        staleController->stop();
        staleController->close();
    }

    if (controller) {
        // Keep existing behavior: calling connectController can re-start a controller.
        controller->start();
    }

    return controller;
}

void HeliosManager::closeAll() {
    std::unordered_map<std::string, std::shared_ptr<HeliosController>> snapshot;
    {
        std::lock_guard lock(activeMutex);
        snapshot = core::snapshotActiveControllersAndClear(activeControllers);
    }

    for (auto& [name, dev] : snapshot) {
        (void)name;
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
