#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/lasercubeusb/LaserCubeUsbController.hpp"
#include "libera/lasercubeusb/LaserCubeUsbControllerInfo.hpp"

#include <memory>

struct libusb_context;

namespace libera::lasercubeusb {

class LaserCubeUsbManager
    : public core::ControllerManagerBase<LaserCubeUsbControllerInfo,
                                         LaserCubeUsbController> {
public:
    LaserCubeUsbManager();
    ~LaserCubeUsbManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

    static core::ControllerManagerRegistry registrar;

private:
    std::shared_ptr<libusb_context> usbContext;

    ControllerPtr createController(const LaserCubeUsbControllerInfo& info) override;
    NewControllerDisposition prepareNewController(LaserCubeUsbController& controller,
                                                  const LaserCubeUsbControllerInfo& info) override;
    void closeController(const std::string& key, LaserCubeUsbController& controller) override;
};

inline core::ControllerManagerRegistry LaserCubeUsbManager::registrar{
    [] { return std::make_unique<LaserCubeUsbManager>(); }
};

} // namespace libera::lasercubeusb
