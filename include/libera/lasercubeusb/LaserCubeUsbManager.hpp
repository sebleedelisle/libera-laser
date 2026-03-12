#pragma once

#include "libera/core/GlobalDacManager.hpp"
#include "libera/lasercubeusb/LaserCubeUsbController.hpp"
#include "libera/lasercubeusb/LaserCubeUsbControllerInfo.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

struct libusb_context;

namespace libera::lasercubeusb {

class LaserCubeUsbManager : public core::DacManagerBase {
public:
    LaserCubeUsbManager();
    ~LaserCubeUsbManager() override;

    std::vector<std::unique_ptr<core::DacInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> getAndConnectToDac(const core::DacInfo& info) override;
    void closeAll() override;

    static inline core::DacManagerRegistry registrar{
        [] { return std::make_unique<LaserCubeUsbManager>(); }
    };

private:
    static constexpr std::string_view typeName{"LaserCubeUSB"};

    std::shared_ptr<libusb_context> usbContext;

    std::mutex activeMutex;
    std::unordered_map<std::string, std::weak_ptr<LaserCubeUsbController>> activeControllers;
};

} // namespace libera::lasercubeusb
