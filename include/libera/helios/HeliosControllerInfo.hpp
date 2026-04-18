#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::helios {

class HeliosControllerInfo : public core::ControllerInfo {
public:
    static constexpr std::string_view controllerType() {
        return "Helios";
    }

    HeliosControllerInfo(std::string id,
                         std::string label,
                         std::uint32_t maxPointRateValue,
                         std::string usbPortPathValue,
                         bool usbController,
                         int firmwareVersionValue)
    : ControllerInfo(controllerType(),
                     std::move(id),
                     std::move(label),
                     maxPointRateValue)
    , usbPortPath(std::move(usbPortPathValue))
    , isUsb(usbController)
    , firmwareVersion(firmwareVersionValue) {}

    // For Helios USB the persistent DAC name is the stable identity, while the
    // port path remains the current transport locator used for the actual USB
    // connection attempt.
    //
    // Why:
    // - users expect the same named DAC to keep its assignment if the cable
    //   moves to another USB port
    // - the bundled SDK's transient indices are not a safe identity either
    // - direct libusb still needs the current port path so we can open the
    //   exact physical DAC discovered on this scan
    const std::string& portPath() const { return usbPortPath; }
    bool isUsbController() const { return isUsb; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    std::string usbPortPath;
    bool isUsb = false;
    int firmwareVersion = 0;
};

} // namespace libera::helios
