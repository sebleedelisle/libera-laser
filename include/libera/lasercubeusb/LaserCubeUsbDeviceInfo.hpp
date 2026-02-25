#pragma once

#include "libera/core/GlobalDacManager.hpp"

#include <string>

namespace libera::lasercubeusb {

class LaserCubeUsbDeviceInfo : public core::DacInfo {
public:
    LaserCubeUsbDeviceInfo(std::string serial,
                           std::string label,
                           std::uint32_t maxPointRateValue = 0)
    : DacInfo(std::move(serial), std::move(label), maxPointRateValue)
    , serialNumber(idValue()) {}

    const std::string& type() const override { return typeName; }

    const std::string& serial() const { return serialNumber; }

private:
    static inline const std::string typeName{"lasercube_usb"};
    std::string serialNumber;
};

} // namespace libera::lasercubeusb
