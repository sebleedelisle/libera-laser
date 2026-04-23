#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::idn {

class IdnControllerInfo : public core::ControllerInfo {
public:
    static constexpr std::string_view controllerType() {
        return "IDN";
    }

    IdnControllerInfo(std::string id,
                      std::string unitIdValue,
                      std::string label,
                      std::uint32_t maxPointRateValue,
                      unsigned int controllerIndexValue,
                      int firmwareVersionValue,
                      std::optional<core::ControllerInfo::NetworkInfo> networkInfo = std::nullopt,
                      std::string hostNameValue = {},
                      std::string serviceNameValue = {})
    : ControllerInfo(controllerType(),
                     std::move(id),
                     std::move(label),
                     maxPointRateValue,
                     std::move(networkInfo))
    , unitIdString(std::move(unitIdValue))
    , controllerIndex(controllerIndexValue)
    , firmwareVersion(firmwareVersionValue)
    , hostNameString(std::move(hostNameValue))
    , serviceNameString(std::move(serviceNameValue)) {}

    const std::string& unitId() const { return unitIdString; }
    unsigned int index() const { return controllerIndex; }
    int firmwareVersionValue() const { return firmwareVersion; }
    const std::string& hostName() const { return hostNameString; }
    const std::string& serviceName() const { return serviceNameString; }

private:
    std::string unitIdString;
    unsigned int controllerIndex = 0;
    int firmwareVersion = 0;
    std::string hostNameString;
    std::string serviceNameString;
};

} // namespace libera::idn
