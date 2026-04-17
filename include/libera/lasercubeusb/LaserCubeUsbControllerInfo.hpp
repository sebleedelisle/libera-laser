#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::lasercubeusb {

class LaserCubeUsbControllerInfo : public core::ControllerInfo {
public:
    static constexpr std::string_view controllerType() {
        return "LaserCubeUSB";
    }

    LaserCubeUsbControllerInfo(std::string serial,
                               std::string label,
                               std::uint32_t maxPointRateValue = 0)
    : ControllerInfo(controllerType(),
                     std::move(serial),
                     std::move(label),
                     maxPointRateValue)
    , serialNumber(idValue()) {}

    const std::string& serial() const { return serialNumber; }

private:
    std::string serialNumber;
};

} // namespace libera::lasercubeusb
