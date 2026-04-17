#pragma once

#include "libera/System.hpp"
#include "libera/core/ControllerCache.hpp"
#include "libera/lasercubeusb/LaserCubeUsbController.hpp"
#include "libera/lasercubeusb/LaserCubeUsbControllerInfo.hpp"

#include <memory>

struct libusb_context;

namespace libera::lasercubeusb {

class LaserCubeUsbManager : public core::ControllerManagerBase {
public:
    LaserCubeUsbManager();
    ~LaserCubeUsbManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info) override;
    void closeAll() override;

    static core::ControllerManagerRegistry registrar;

private:
    static constexpr std::string_view typeName{"LaserCubeUSB"};

    std::shared_ptr<libusb_context> usbContext;

    core::ControllerCache<std::string, LaserCubeUsbController> activeControllers;
};

inline core::ControllerManagerRegistry LaserCubeUsbManager::registrar{
    [] { return std::make_unique<LaserCubeUsbManager>(); }
};

} // namespace libera::lasercubeusb
