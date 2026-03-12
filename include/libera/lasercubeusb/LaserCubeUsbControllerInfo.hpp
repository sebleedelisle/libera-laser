#pragma once

#include "libera/core/GlobalDacManager.hpp"

#include <string>

namespace libera::lasercubeusb {

class LaserCubeUsbControllerInfo : public core::DacInfo {
public:
    LaserCubeUsbControllerInfo(std::string serial,
                           std::string label,
                           std::uint32_t maxPointRateValue = 0)
    : DacInfo(std::move(serial), std::move(label), maxPointRateValue)
    , serialNumber(idValue()) {}

    const std::string& type() const override { return typeName; }

    const std::string& serial() const { return serialNumber; }

private:
    static inline const std::string typeName{"LaserCubeUSB"};
    std::string serialNumber;
};

} // namespace libera::lasercubeusb
