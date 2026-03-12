#include "libera/helios/HeliosManager.hpp"

#include "libera/log/Log.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace libera::helios {
namespace {

std::string makeFallbackLabel(unsigned int index) {
    return "Helios " + std::to_string(index);
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
    std::unordered_set<std::string> usedIds;
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
        const bool usbController = isUsb == 1;
        const std::uint32_t maxRate = usbController ? HELIOS_MAX_PPS : HELIOS_MAX_PPS_IDN;
        const int firmware = sdk->GetFirmwareVersion(index);
        std::string id = makeHeliosControllerId(label, index, usedIds);

        results.emplace_back(std::make_unique<HeliosControllerInfo>(
            std::move(id),
            std::move(label),
            maxRate,
            index,
            usbController,
            firmware));
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
        auto it = activeControllers.find(heliosInfo->index());
        if (it != activeControllers.end()) {
            if (auto existing = it->second.lock()) {
                controller = existing;
            } else {
                activeControllers.erase(it);
            }
        }

        if (!controller) {
            controller = std::make_shared<HeliosController>(sdk, heliosInfo->index());
            activeControllers[heliosInfo->index()] = controller;
        }
    }

    if (controller) {
        controller->start();
    }

    return controller;
}

void HeliosManager::closeAll() {
    std::unordered_map<unsigned int, std::shared_ptr<HeliosController>> snapshot;
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

} // namespace libera::helios
