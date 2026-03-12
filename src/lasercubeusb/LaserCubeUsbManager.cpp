#include "libera/lasercubeusb/LaserCubeUsbManager.hpp"

#include "libera/core/ActiveControllerMap.hpp"
#include "libera/lasercubeusb/LaserCubeUsbConfig.hpp"
#include "libera/log/Log.hpp"

#ifdef _WIN32
#define _WINSOCKAPI_
#endif
#include "libusb.h"

#include <optional>

namespace libera::lasercubeusb {
namespace {

std::shared_ptr<libusb_context> createContext() {
    libusb_context* raw = nullptr;
    const int rc = libusb_init(&raw);
    if (rc != 0 || !raw) {
        logError("[LaserCubeUsbManager] libusb_init failed", libusb_error_name(rc));
        return {};
    }
    return std::shared_ptr<libusb_context>(raw, [](libusb_context* ctx) {
        if (ctx) {
            libusb_exit(ctx);
        }
    });
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

} // namespace

LaserCubeUsbManager::LaserCubeUsbManager() {
    usbContext = createContext();
}

LaserCubeUsbManager::~LaserCubeUsbManager() {
    closeAll();
    usbContext.reset();
}

std::vector<std::unique_ptr<core::DacInfo>> LaserCubeUsbManager::discover() {
    std::vector<std::unique_ptr<core::DacInfo>> results;
    if (!usbContext) {
        return results;
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
        results.emplace_back(std::make_unique<LaserCubeUsbControllerInfo>(*serial, label));
    }

    libusb_free_device_list(deviceList, 1);
    return results;
}

std::shared_ptr<core::LaserController>
LaserCubeUsbManager::getAndConnectToDac(const core::DacInfo& info) {
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
    controller->start();

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
            controller->stop();
            controller->close();
        }
    }
}

} // namespace libera::lasercubeusb
