#pragma once

#include "libera/core/GlobalDacManager.hpp"

#include <string>

namespace libera::helios {

class HeliosDeviceInfo : public core::DacInfo {
public:
    HeliosDeviceInfo(std::string id,
                     std::string label,
                     std::uint32_t maxPointRateValue,
                     unsigned int deviceIndexValue,
                     bool usbDevice,
                     int firmwareVersionValue)
    : DacInfo(std::move(id), std::move(label), maxPointRateValue)
    , deviceIndex(deviceIndexValue)
    , isUsb(usbDevice)
    , firmwareVersion(firmwareVersionValue) {}

    const std::string& type() const override { return typeName; }

    unsigned int index() const { return deviceIndex; }
    bool isUsbDevice() const { return isUsb; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    static inline const std::string typeName{"helios"};

    unsigned int deviceIndex = 0;
    bool isUsb = false;
    int firmwareVersion = 0;
};

} // namespace libera::helios
