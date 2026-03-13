#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::helios {

class HeliosControllerInfo : public core::ControllerInfo {
public:
    HeliosControllerInfo(std::string id,
                     std::string label,
                     std::uint32_t maxPointRateValue,
                     unsigned int controllerIndexValue,
                     bool usbController,
                     int firmwareVersionValue)
    : ControllerInfo(std::move(id), std::move(label), maxPointRateValue)
    , controllerIndex(controllerIndexValue)
    , isUsb(usbController)
    , firmwareVersion(firmwareVersionValue) {}

    const std::string& type() const override { return typeName; }

    unsigned int index() const { return controllerIndex; }
    bool isUsbController() const { return isUsb; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    static inline const std::string typeName{"Helios"};

    unsigned int controllerIndex = 0;
    bool isUsb = false;
    int firmwareVersion = 0;
};

} // namespace libera::helios
