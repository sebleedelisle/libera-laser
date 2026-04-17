#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::helios {

class HeliosControllerInfo : public core::ControllerInfo {
public:
    HeliosControllerInfo(std::string id,
                         std::string label,
                         std::uint32_t maxPointRateValue,
                         std::string usbPortPathValue,
                         bool usbController,
                         int firmwareVersionValue)
    : ControllerInfo("Helios",
                     std::move(id),
                     std::move(label),
                     maxPointRateValue)
    , usbPortPath(std::move(usbPortPathValue))
    , isUsb(usbController)
    , firmwareVersion(firmwareVersionValue) {}

    // For Helios USB we intentionally identify the physical device by USB port
    // path rather than a vendor-SDK index.
    //
    // Why:
    // - the bundled SDK opens every Helios USB DAC at once and assigns transient
    //   indices inside one process-local device list
    // - when another process is already using one Helios, those indices are no
    //   longer stable or even globally meaningful
    // - a port path lets us reconnect to the exact physical DAC we discovered
    //   without forcing a process to claim all Helios devices first
    const std::string& portPath() const { return usbPortPath; }
    bool isUsbController() const { return isUsb; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    std::string usbPortPath;
    bool isUsb = false;
    int firmwareVersion = 0;
};

} // namespace libera::helios
