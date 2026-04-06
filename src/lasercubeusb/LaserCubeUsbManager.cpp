#include "libera/lasercubeusb/LaserCubeUsbManager.hpp"

#include "libera/core/ActiveControllerMap.hpp"
#include "libera/lasercubeusb/LaserCubeUsbConfig.hpp"
#include "libera/log/Log.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // Keep Windows headers from defining min/max macros that break std::min/std::max.
#endif
#define _WINSOCKAPI_
#endif
#include "libusb.h"

#include <optional>
#include <unordered_set>

namespace libera::lasercubeusb {
namespace {

std::shared_ptr<libusb_context> createContext() {
    libusb_context* raw = nullptr;
    const int rc = libusb_init(&raw);
    if (rc != 0 || !raw) {
        logError("[LaserCubeUsbManager] libusb_init failed", libusb_error_name(rc));
        return {};
    }

    // Shutdown strategy:
    // Helios SDK uses detached libusb worker threads and the default libusb
    // context. During app teardown that can race with additional libusb_exit()
    // calls from other managers and crash inside libusb's context list logic.
    //
    // Keep the LaserCube USB context process-lifetime instead of explicitly
    // calling libusb_exit(ctx) during manager destruction. The OS reclaims this
    // on process exit, and we avoid the teardown race.
    return std::shared_ptr<libusb_context>(raw, [](libusb_context*) {});
}

bool isLaserCubeUsb(const libusb_device_descriptor& descriptor) {
    return descriptor.idVendor == LaserCubeUsbConfig::VENDOR_ID &&
           descriptor.idProduct == LaserCubeUsbConfig::PRODUCT_ID;
}

std::optional<std::string> readSerialNumber(libusb_device* controller) {
    libusb_device_descriptor descriptor{};
    const int rc = libusb_get_device_descriptor(controller, &descriptor);
    if (rc != 0) {
        return std::nullopt;
    }

    if (!isLaserCubeUsb(descriptor)) {
        return std::nullopt;
    }

    if (descriptor.iSerialNumber == 0) {
        return std::nullopt;
    }

    libusb_device_handle* handle = nullptr;
    if (libusb_open(controller, &handle) != 0 || !handle) {
        return std::nullopt;
    }

    unsigned char buffer[256] = {};
    const int length = libusb_get_string_descriptor_ascii(
        handle,
        descriptor.iSerialNumber,
        buffer,
        static_cast<int>(sizeof(buffer)));

    libusb_close(handle);

    if (length <= 0) {
        return std::nullopt;
    }

    return std::string(reinterpret_cast<char*>(buffer), static_cast<std::size_t>(length));
}

core::ControllerUsageState probeUsageState(libusb_device* controller, bool ownedByThisProcess) {
    if (ownedByThisProcess) {
        // Discovery should not mark a LaserCube USB DAC as externally busy when
        // this same process already owns it.
        return core::ControllerUsageState::Idle;
    }

    libusb_device_handle* handle = nullptr;
    const int openRc = libusb_open(controller, &handle);
    if (openRc == LIBUSB_ERROR_BUSY || openRc == LIBUSB_ERROR_ACCESS) {
        return core::ControllerUsageState::BusyExclusive;
    }
    if (openRc != LIBUSB_SUCCESS || !handle) {
        return core::ControllerUsageState::Unknown;
    }

    bool claimedInterface0 = false;
    bool claimedInterface1 = false;
    auto cleanup = [&] {
        if (claimedInterface1) {
            libusb_release_interface(handle, 1);
        }
        if (claimedInterface0) {
            libusb_release_interface(handle, 0);
        }
        libusb_close(handle);
    };

    // Match the real LaserCube USB connect path closely enough to detect when
    // another app has exclusive ownership, but release everything immediately so
    // discovery itself remains non-owning.
    const int claim0Rc = libusb_claim_interface(handle, 0);
    if (claim0Rc != LIBUSB_SUCCESS) {
        cleanup();
        return core::ControllerUsageState::BusyExclusive;
    }
    claimedInterface0 = true;

    const int claim1Rc = libusb_claim_interface(handle, 1);
    if (claim1Rc != LIBUSB_SUCCESS) {
        cleanup();
        return core::ControllerUsageState::BusyExclusive;
    }
    claimedInterface1 = true;

    const int altRc = libusb_set_interface_alt_setting(handle, 1, 1);
    if (altRc != LIBUSB_SUCCESS) {
        cleanup();
        return core::ControllerUsageState::BusyExclusive;
    }

    cleanup();
    return core::ControllerUsageState::Idle;
}

} // namespace

LaserCubeUsbManager::LaserCubeUsbManager() {
    usbContext = createContext();
}

LaserCubeUsbManager::~LaserCubeUsbManager() {
    closeAll();
    usbContext.reset();
}

std::vector<std::unique_ptr<core::ControllerInfo>> LaserCubeUsbManager::discover() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    if (!usbContext) {
        return results;
    }

    std::unordered_set<std::string> connectedSerials;
    {
        std::lock_guard<std::mutex> lock(activeMutex);
        core::pruneExpiredActiveControllers(activeControllers);
        for (const auto& [serial, weakController] : activeControllers) {
            const auto controller = weakController.lock();
            if (!controller) {
                continue;
            }
            if (controller->getStatus() != core::ControllerStatus::Error) {
                connectedSerials.insert(serial);
            }
        }
    }

    libusb_device** deviceList = nullptr;
    const ssize_t count = libusb_get_device_list(usbContext.get(), &deviceList);
    if (count < 0 || !deviceList) {
        logError("[LaserCubeUsbManager] libusb_get_device_list failed", libusb_error_name(static_cast<int>(count)));
        return results;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* controller = deviceList[i];
        auto serial = readSerialNumber(controller);
        if (!serial) {
            continue;
        }
        const std::string label = serial->empty() ? std::string("LaserCube USB")
                                                  : std::string("LaserCube USB ") + *serial;
        auto info = std::make_unique<LaserCubeUsbControllerInfo>(*serial, label);
        info->setUsageState(
            probeUsageState(controller, connectedSerials.find(*serial) != connectedSerials.end()));
        results.emplace_back(std::move(info));
    }

    libusb_free_device_list(deviceList, 1);
    return results;
}

std::shared_ptr<core::LaserController>
LaserCubeUsbManager::connectController(const core::ControllerInfo& info) {
    const auto* usbInfo = dynamic_cast<const LaserCubeUsbControllerInfo*>(&info);
    if (!usbInfo || !usbContext) {
        return nullptr;
    }

    bool newlyCreated = false;
    std::shared_ptr<LaserCubeUsbController> controller;
    {
        std::lock_guard<std::mutex> lock(activeMutex);
        controller = core::getOrCreateActiveController(
            activeControllers,
            usbInfo->idValue(),
            [this] { return std::make_shared<LaserCubeUsbController>(usbContext); },
            &newlyCreated);
    }

    if (!controller) {
        return nullptr;
    }
    // If a live controller already exists, reuse it and avoid reconnecting USB.
    if (!newlyCreated) {
        return controller;
    }

    const auto result = controller->connect(*usbInfo);
    if (!result) {
        logError("[LaserCubeUsbManager] connect failed", result.error().message());
        std::lock_guard<std::mutex> lock(activeMutex);
        activeControllers.erase(usbInfo->idValue());
        return nullptr;
    }
    controller->startThread();

    return controller;
}

void LaserCubeUsbManager::closeAll() {
    std::unordered_map<std::string, std::shared_ptr<LaserCubeUsbController>> snapshot;
    {
        std::lock_guard<std::mutex> lock(activeMutex);
        snapshot = core::snapshotActiveControllersAndClear(activeControllers);
    }

    for (auto& [id, controller] : snapshot) {
        if (controller) {
            controller->stopThread();
            controller->close();
        }
    }
}

} // namespace libera::lasercubeusb
