#include "libera/helios/HeliosManager.hpp"

#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace libera::helios {
namespace {

constexpr std::size_t maxHeliosNameLength = 30;

std::string truncateHeliosName(const std::string& label);

std::string trimAsciiWhitespace(std::string value) {
    auto isAsciiSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    while (!value.empty() && isAsciiSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isAsciiSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string lowercaseAscii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

bool containsAsciiNoCase(std::string_view haystack, std::string_view needle) {
    return lowercaseAscii(haystack).find(lowercaseAscii(needle)) != std::string::npos;
}

std::string sanitizeSerialToken(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string digitsOnly(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (std::isdigit(ch)) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string makePortPathFallbackToken(const std::string& portPath, std::size_t maxLength) {
    std::string digits = digitsOnly(portPath);
    if (digits.empty()) {
        digits = sanitizeSerialToken(portPath);
    }
    if (digits.empty()) {
        digits = "0";
    }
    if (digits.size() > maxLength) {
        digits = digits.substr(digits.size() - maxLength);
    }
    return digits;
}

std::string queryUsbStringDescriptor(libusb_device_handle* handle, std::uint8_t descriptorIndex) {
    if (handle == nullptr || descriptorIndex == 0) {
        return {};
    }

    std::array<unsigned char, 128> buffer{};
    const int length = libusb_get_string_descriptor_ascii(handle,
                                                          descriptorIndex,
                                                          buffer.data(),
                                                          static_cast<int>(buffer.size()));
    if (length <= 0) {
        return {};
    }

    return trimAsciiWhitespace(
        std::string(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(length)));
}

bool prefersHeliosProDefaultName(const std::string& productString,
                                 const std::string& serialToken) {
    if (containsAsciiNoCase(productString, "heliospro") ||
        containsAsciiNoCase(productString, "idn")) {
        return true;
    }

    // The newer HeliosPro-style default names typically use a short numeric
    // suffix, while original Helios USB names usually carry a longer serial.
    return !serialToken.empty() && serialToken.size() <= 4;
}

std::string makeHeliosUsbDefaultName(const std::string& productString,
                                     const std::string& serialString,
                                     const std::string& portPath) {
    std::string serialToken = digitsOnly(serialString);
    if (serialToken.empty()) {
        serialToken = sanitizeSerialToken(serialString);
    }

    if (prefersHeliosProDefaultName(productString, serialToken)) {
        if (serialToken.empty()) {
            serialToken = makePortPathFallbackToken(portPath, 4);
        } else if (serialToken.size() > 4) {
            serialToken = serialToken.substr(serialToken.size() - 4);
        }
        return truncateHeliosName("HeliosPro " + serialToken);
    }

    if (serialToken.empty()) {
        serialToken = makePortPathFallbackToken(portPath, 9);
    }
    return truncateHeliosName("helios " + serialToken);
}

std::string makeGenericHeliosFallbackName(unsigned int index) {
    return truncateHeliosName("helios " + std::to_string(index));
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
        base = makeGenericHeliosFallbackName(index);
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
            base = makeGenericHeliosFallbackName(index);
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
        candidate = makeGenericHeliosFallbackName(index);
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

std::string makeHeliosUsbPortPath(libusb_device* device) {
    if (!device) {
        return "unknown";
    }

    std::array<std::uint8_t, 8> ports{};
    const int depth = libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
    if (depth > 0) {
        std::string path;
        for (int i = 0; i < depth; ++i) {
            if (!path.empty()) {
                path += "-";
            }
            path += std::to_string(static_cast<unsigned>(ports[i]));
        }
        if (!path.empty()) {
            return path;
        }
    }

    return "bus" + std::to_string(static_cast<unsigned>(libusb_get_bus_number(device))) +
           "-dev" + std::to_string(static_cast<unsigned>(libusb_get_device_address(device)));
}

std::string makeBusyHeliosControllerId(const std::string& portPath) {
    return "helios-usb-busy-" + portPath;
}

std::string makeBusyHeliosLabel(const std::string& portPath) {
    return "Helios USB " + portPath;
}

class ScopedHeliosUsbHandle {
public:
    explicit ScopedHeliosUsbHandle(libusb_device* device) {
        if (!device) {
            return;
        }

        const int openRc = libusb_open(device, &handle);
        // Discovery policy:
        // if we cannot even open the device, treat that as "somebody else owns
        // this Helios" rather than silently dropping it from the list.
        //
        // In practice macOS/libusb may surface either BUSY or ACCESS when
        // another process is already holding the interface.
        if (openRc == LIBUSB_ERROR_BUSY || openRc == LIBUSB_ERROR_ACCESS) {
            busyExclusiveValue = true;
            return;
        }
        if (openRc != LIBUSB_SUCCESS || handle == nullptr) {
            return;
        }

        const int claimRc = libusb_claim_interface(handle, 0);
        // Important: unlike LaserCube USB, the legacy Helios SDK claims every
        // matching DAC it sees. Here we only probe one specific device at a
        // time, and any failure to claim that interface is treated as evidence
        // that this DAC is already in use.
        if (claimRc != LIBUSB_SUCCESS) {
            busyExclusiveValue = true;
            libusb_close(handle);
            handle = nullptr;
            return;
        }
        interfaceClaimed = true;

        const int altRc = libusb_set_interface_alt_setting(handle, 0, 1);
        // The Helios USB firmware expects alt setting 1. If another process has
        // already configured or monopolized the interface and we cannot switch
        // into that mode, keep surfacing the DAC as externally busy.
        if (altRc != LIBUSB_SUCCESS) {
            busyExclusiveValue = true;
            libusb_release_interface(handle, 0);
            interfaceClaimed = false;
            libusb_close(handle);
            handle = nullptr;
            return;
        }

        std::array<std::uint8_t, 32> flushBuffer{};
        int actualLength = 0;
        while (libusb_interrupt_transfer(
                   handle,
                   EP_INT_IN,
                   flushBuffer.data(),
                   static_cast<int>(flushBuffer.size()),
                   &actualLength,
                   5) == LIBUSB_SUCCESS) {
        }
    }

    ~ScopedHeliosUsbHandle() {
        if (handle != nullptr && interfaceClaimed) {
            libusb_release_interface(handle, 0);
        }
        if (handle != nullptr) {
            libusb_close(handle);
        }
    }

    bool valid() const {
        return handle != nullptr && interfaceClaimed;
    }

    bool busyExclusive() const {
        return busyExclusiveValue;
    }

    libusb_device_handle* get() const {
        return handle;
    }

private:
    libusb_device_handle* handle = nullptr;
    bool interfaceClaimed = false;
    bool busyExclusiveValue = false;
};

bool heliosInterruptOut(libusb_device_handle* handle,
                        const std::uint8_t* buffer,
                        int length,
                        unsigned int timeoutMs = 32) {
    if (handle == nullptr || buffer == nullptr || length <= 0) {
        return false;
    }

    int actualLength = 0;
    const int rc = libusb_interrupt_transfer(handle,
                                             EP_INT_OUT,
                                             const_cast<unsigned char*>(buffer),
                                             length,
                                             &actualLength,
                                             timeoutMs);
    return rc == LIBUSB_SUCCESS && actualLength == length;
}

int queryHeliosUsbFirmwareVersion(libusb_device_handle* handle) {
    if (handle == nullptr) {
        return 0;
    }

    for (int i = 0; i < 2; ++i) {
        const std::uint8_t request[2] = {0x04, 0};
        if (!heliosInterruptOut(handle, request, 2)) {
            continue;
        }

        for (int j = 0; j < 3; ++j) {
            std::array<std::uint8_t, 32> response{};
            int actualLength = 0;
            const int rc = libusb_interrupt_transfer(handle,
                                                     EP_INT_IN,
                                                     response.data(),
                                                     static_cast<int>(response.size()),
                                                     &actualLength,
                                                     32);
            if (rc != LIBUSB_SUCCESS || actualLength < 5 || response[0] != 0x84) {
                continue;
            }

            return (response[1] << 0) |
                   (response[2] << 8) |
                   (response[3] << 16) |
                   (response[4] << 24);
        }
    }

    return 0;
}

void announceHeliosSdkVersion(libusb_device_handle* handle) {
    if (handle == nullptr) {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        const std::uint8_t request[2] = {0x07, HELIOS_SDK_VERSION};
        if (heliosInterruptOut(handle, request, 2)) {
            return;
        }
    }
}

struct HeliosUsbNameQueryResult {
    bool succeeded = false;
    bool empty = false;
    std::string label;
};

HeliosUsbNameQueryResult queryHeliosUsbName(libusb_device_handle* handle) {
    HeliosUsbNameQueryResult result;
    if (handle == nullptr) {
        return result;
    }

    for (int i = 0; i < 2; ++i) {
        const std::uint8_t request[2] = {0x05, 0};
        if (!heliosInterruptOut(handle, request, 2)) {
            continue;
        }

        std::array<std::uint8_t, 32> response{};
        int actualLength = 0;
        const int rc = libusb_interrupt_transfer(handle,
                                                 EP_INT_IN,
                                                 response.data(),
                                                 static_cast<int>(response.size()),
                                                 &actualLength,
                                                 32);
        if (rc != LIBUSB_SUCCESS || actualLength < 2 || response[0] != 0x85) {
            continue;
        }

        response.back() = 0;
        const char* nameData = reinterpret_cast<const char*>(response.data() + 1);
        std::size_t length = 0;
        while (length < 30 && nameData[length] != '\0') {
            ++length;
        }

        result.succeeded = true;
        result.label.assign(nameData, length);
        result.empty = result.label.empty();
        return result;
    }

    return result;
}

bool setHeliosUsbName(libusb_device_handle* handle, const std::string& newLabel) {
    if (handle == nullptr) {
        return false;
    }

    std::array<std::uint8_t, 32> request{};
    request[0] = 0x06;
    const std::string safeName = truncateHeliosName(newLabel);
    std::memcpy(request.data() + 1, safeName.c_str(), std::min<std::size_t>(safeName.size(), 30));
    return heliosInterruptOut(handle, request.data(), static_cast<int>(request.size()));
}

struct DirectHeliosUsbProbe {
    libusb_device* device = nullptr;
    std::string portPath;
    std::string reportedLabel;
    std::string productString;
    std::string serialString;
    int firmwareVersion = 0;
    bool nameQuerySucceeded = false;
    bool nameWasEmpty = false;
};

std::shared_ptr<libusb_context> createUsbContext() {
    libusb_context* raw = nullptr;
    const int rc = libusb_init(&raw);
    if (rc != LIBUSB_SUCCESS || raw == nullptr) {
        logError("[HeliosManager] libusb_init failed", libusb_error_name(rc));
        return {};
    }

    // Shutdown strategy:
    // keep the Helios USB libusb context process-lifetime. Explicit libusb_exit
    // during app teardown has already shown up as crash-prone with USB worker
    // threads still unwinding elsewhere in the stack.
    //
    // The goal here is to avoid a clean-looking teardown path that is actually
    // less safe than leaking a tiny amount of process-lifetime state.
    return std::shared_ptr<libusb_context>(raw, [](libusb_context*) {});
}

} // namespace

HeliosManager::HeliosManager() {
    usbContext = createUsbContext();
}

HeliosManager::~HeliosManager() {
    closeAll();
}

HeliosManager::ActiveControllerSnapshot HeliosManager::snapshotActiveControllers() {
    ActiveControllerSnapshot snapshot;
    const auto activeControllerSnapshot = liveControllers();
    snapshot.hasActive = !activeControllerSnapshot.empty();
    if (!snapshot.hasActive) {
        return snapshot;
    }

    for (const auto& [controllerName, controller] : activeControllerSnapshot) {
        (void)controllerName;
        if (controller && controller->isConnected()) {
            // Record the exact DAC this process owns so discovery does not mark
            // our own connected device as "(in use)" by mistake.
            snapshot.connectedPortPaths.insert(controller->controllerPortPath());
        }
    }

    return snapshot;
}

std::vector<HeliosControllerInfo> HeliosManager::collectDiscoveredControllers(
    const ActiveControllerSnapshot& activeSnapshot) {
    std::vector<HeliosControllerInfo> results;

    if (!usbContext) {
        return results;
    }

    // Architectural choice:
    // enumerate Helios USB devices directly through libusb instead of using the
    // bundled Helios SDK for discovery.
    //
    // Why we do this:
    // - the vendor SDK opens and claims every Helios USB DAC it discovers
    // - once one process does that, a second process sees every Helios as busy
    // - we want one connected DAC to make only that DAC unavailable, not the
    //   whole Helios fleet
    //
    // So discovery is now a lightweight direct probe, and only connect() takes
    // exclusive ownership of a single chosen DAC.
    libusb_device** deviceList = nullptr;
    const ssize_t deviceCount = libusb_get_device_list(usbContext.get(), &deviceList);
    if (deviceCount < 0 || deviceList == nullptr) {
        return results;
    }

    std::vector<DirectHeliosUsbProbe> freeProbes;
    std::vector<DirectHeliosUsbProbe> busyProbes;
    std::unordered_set<std::string> seenPortPaths;

    for (ssize_t i = 0; i < deviceCount; ++i) {
        libusb_device* device = deviceList[i];
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(device, &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != HELIOS_VID || descriptor.idProduct != HELIOS_PID) {
            continue;
        }

        DirectHeliosUsbProbe probe;
        probe.device = device;
        probe.portPath = makeHeliosUsbPortPath(device);
        seenPortPaths.insert(probe.portPath);
        const bool ownedByThisProcess =
            activeSnapshot.connectedPortPaths.find(probe.portPath) != activeSnapshot.connectedPortPaths.end();

        ScopedHeliosUsbHandle handle(device);
        if (handle.busyExclusive()) {
            // Distinguish "busy because we own it" from "busy because somebody
            // else owns it". Without this branch, a DAC already connected in the
            // current process would bounce in and out of the assigner as busy.
            if (ownedByThisProcess) {
                freeProbes.push_back(std::move(probe));
            } else {
                busyProbes.push_back(std::move(probe));
            }
            continue;
        }
        if (!handle.valid()) {
            continue;
        }

        probe.firmwareVersion = queryHeliosUsbFirmwareVersion(handle.get());
        announceHeliosSdkVersion(handle.get());
        probe.productString = queryUsbStringDescriptor(handle.get(), descriptor.iProduct);
        probe.serialString = queryUsbStringDescriptor(handle.get(), descriptor.iSerialNumber);
        const HeliosUsbNameQueryResult nameResult = queryHeliosUsbName(handle.get());
        probe.nameQuerySucceeded = nameResult.succeeded;
        probe.nameWasEmpty = nameResult.empty;
        probe.reportedLabel = nameResult.label;
        freeProbes.push_back(std::move(probe));
    }

    auto freeProbeOrder = [](const DirectHeliosUsbProbe& a, const DirectHeliosUsbProbe& b) {
        if (a.reportedLabel != b.reportedLabel) {
            return a.reportedLabel < b.reportedLabel;
        }
        return a.portPath < b.portPath;
    };
    std::sort(freeProbes.begin(), freeProbes.end(), freeProbeOrder);
    std::sort(busyProbes.begin(),
              busyProbes.end(),
              [](const DirectHeliosUsbProbe& a, const DirectHeliosUsbProbe& b) {
                  return a.portPath < b.portPath;
              });

    std::unordered_set<std::string> usedIds;
    std::unordered_set<std::string> usedLabels;
    results.reserve(freeProbes.size() + busyProbes.size());

    for (std::size_t i = 0; i < freeProbes.size(); ++i) {
        DirectHeliosUsbProbe& probe = freeProbes[i];
        std::string label = probe.reportedLabel;
        const bool hadCachedLabel = stableLabelByPortPath.find(probe.portPath) != stableLabelByPortPath.end();
        const std::string preferredDefaultName =
            makeHeliosUsbDefaultName(probe.productString, probe.serialString, probe.portPath);

        if (label.empty()) {
            auto cachedLabel = stableLabelByPortPath.find(probe.portPath);
            if (cachedLabel != stableLabelByPortPath.end()) {
                label = cachedLabel->second;
            }
        }

        if (label.empty()) {
            label = preferredDefaultName;
        }

        const bool duplicateLabel = usedLabels.find(label) != usedLabels.end();
        // Only attempt a hardware rename when the device name is genuinely
        // new-empty (no cached label from a previous scan) or a true duplicate.
        // Without the cache check, DACs that don't persist names to flash would
        // trigger a rename on every single discovery cycle.
        const bool shouldPersistUniqueLabel = duplicateLabel || (probe.nameWasEmpty && !hadCachedLabel);
        if (shouldPersistUniqueLabel) {
            // Empty or duplicate USB Helios names are unsafe as controller
            // identities, so rewrite them to the same style as the vendor's
            // default persistent names before exposing the device.
            const std::string originalDeviceLabel = probe.nameWasEmpty ? std::string() : label;
            const std::string renamedLabel =
                makeUniqueRenameLabel(static_cast<unsigned int>(i),
                                      preferredDefaultName,
                                      originalDeviceLabel,
                                      usedLabels);
            ScopedHeliosUsbHandle renameHandle(probe.device);
            if (renameHandle.valid() && setHeliosUsbName(renameHandle.get(), renamedLabel)) {
                logInfo("[HeliosManager] Helios name auto-renamed",
                        "path", probe.portPath,
                        "reason", probe.nameWasEmpty ? "empty" : "duplicate",
                        "old_label", probe.nameWasEmpty ? std::string("<empty>") : label,
                        "new_label", renamedLabel);
                label = renamedLabel;
            } else {
                logError("[HeliosManager] failed to auto-rename Helios name",
                         "path", probe.portPath,
                         "reason", probe.nameWasEmpty ? "empty" : "duplicate",
                         "label", label,
                         "requested_label", renamedLabel);
            }
        }

        if (probe.nameQuerySucceeded || shouldPersistUniqueLabel || hadCachedLabel) {
            // Cache by physical port path, not by transient discovery order.
            // This keeps the label stable across rescans and across the busy/free
            // transition of the same physical DAC.
            stableLabelByPortPath[probe.portPath] = label;
        }
        usedLabels.insert(label);

        HeliosControllerInfo info(makeHeliosControllerId(label, static_cast<unsigned int>(i), usedIds),
                                  label,
                                  HELIOS_MAX_PPS,
                                  probe.portPath,
                                  true,
                                  probe.firmwareVersion);
        info.setUsageState(core::ControllerUsageState::Idle);
        results.push_back(std::move(info));
    }

    for (std::size_t i = 0; i < busyProbes.size(); ++i) {
        const DirectHeliosUsbProbe& probe = busyProbes[i];
        std::string label = makeBusyHeliosLabel(probe.portPath);
        std::string id = makeBusyHeliosControllerId(probe.portPath);

        auto cachedLabel = stableLabelByPortPath.find(probe.portPath);
        if (cachedLabel != stableLabelByPortPath.end() && !cachedLabel->second.empty()) {
            // If we have previously read the real device name while it was free,
            // keep showing that name when it later becomes busy elsewhere.
            label = cachedLabel->second;
            id = makeHeliosControllerId(label,
                                        static_cast<unsigned int>(freeProbes.size() + i),
                                        usedIds);
        }

        HeliosControllerInfo busyInfo(std::move(id),
                                      std::move(label),
                                      HELIOS_MAX_PPS,
                                      probe.portPath,
                                      true,
                                      0);
        busyInfo.setUsageState(core::ControllerUsageState::BusyExclusive);
        results.push_back(std::move(busyInfo));
    }

    for (auto it = stableLabelByPortPath.begin(); it != stableLabelByPortPath.end();) {
        if (seenPortPaths.find(it->first) == seenPortPaths.end()) {
            it = stableLabelByPortPath.erase(it);
        } else {
            ++it;
        }
    }

    libusb_free_device_list(deviceList, 1);
    return results;
}

std::vector<std::unique_ptr<core::ControllerInfo>> HeliosManager::discover() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    const ActiveControllerSnapshot activeSnapshot = snapshotActiveControllers();
    std::vector<HeliosControllerInfo> discoveredInfos = collectDiscoveredControllers(activeSnapshot);

    results.reserve(discoveredInfos.size());
    for (auto& info : discoveredInfos) {
        results.emplace_back(std::make_unique<HeliosControllerInfo>(std::move(info)));
    }
    return results;
}

std::string
HeliosManager::controllerKey(const HeliosControllerInfo& info) const {
    return info.labelValue();
}

std::shared_ptr<HeliosController>
HeliosManager::createController(const HeliosControllerInfo& info) {
    if (!info.isUsbController() || info.portPath().empty() || !usbContext) {
        return nullptr;
    }

    // Direct USB connect claims only the selected DAC, leaving other
    // Helios DACs visible and connectable from other processes.
    return HeliosController::connectUsb(usbContext, info.portPath());
}

bool HeliosManager::shouldReuseController(const HeliosController& controller,
                                          const HeliosControllerInfo& info) const {
    (void)info;
    return controller.isConnected();
}

HeliosManager::NewControllerDisposition
HeliosManager::prepareNewController(HeliosController& controller,
                                    const HeliosControllerInfo& info) {
    (void)info;
    // Connection policy:
    // connect exactly one physical Helios DAC, identified by port path.
    //
    // This is the main behavioral fix. The old SDK-backed path opened every
    // Helios USB DAC up front, which made all of them look "in use" to any
    // second process even if we had only assigned one.
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void HeliosManager::prepareExistingController(HeliosController& controller,
                                              const HeliosControllerInfo& info) {
    (void)info;
    // Keep existing behavior: calling connectController can re-start a controller.
    controller.startThread();
}

void HeliosManager::closeController(const std::string& key,
                                    HeliosController& controller) {
    (void)key;
    // Shutdown-only escape hatch:
    // the direct Helios USB path has shown libusb_close() crashes during
    // app teardown on macOS. After the worker thread is joined there is no
    // more live I/O in this process, so ask the controller to abandon its
    // raw libusb handle instead of performing the final close call.
    controller.prepareForShutdown();
    controller.close();
}

void HeliosManager::afterCloseControllers() {
    // Intentionally do not destroy or explicitly tear down the shared libusb
    // context here. Process-lifetime ownership is the safer tradeoff for the
    // shutdown crashes we were seeing.
    stableLabelByPortPath.clear();
}

} // namespace libera::helios
