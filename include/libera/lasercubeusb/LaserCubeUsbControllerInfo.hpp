#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::lasercubeusb {

class LaserCubeUsbControllerInfo : public core::ControllerInfo {
public:
    LaserCubeUsbControllerInfo(std::string serial,
                               std::string label,
                               std::uint32_t maxPointRateValue = 0)
    : ControllerInfo("LaserCubeUSB",
                     std::move(serial),
                     std::move(label),
                     maxPointRateValue)
    , serialNumber(idValue()) {}

    const std::string& serial() const { return serialNumber; }

private:
    std::string serialNumber;
};

} // namespace libera::lasercubeusb
