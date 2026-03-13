#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::idn {

class IdnControllerInfo : public core::ControllerInfo {
public:
    IdnControllerInfo(std::string id,
                      std::string unitIdValue,
                      std::string label,
                      std::uint32_t maxPointRateValue,
                      unsigned int controllerIndexValue,
                      int firmwareVersionValue,
                      std::optional<core::ControllerInfo::NetworkInfo> networkInfo = std::nullopt)
    : ControllerInfo(std::move(id), std::move(label), maxPointRateValue, std::move(networkInfo))
    , unitIdString(std::move(unitIdValue))
    , controllerIndex(controllerIndexValue)
    , firmwareVersion(firmwareVersionValue) {}

    const std::string& type() const override { return typeName; }

    const std::string& unitId() const { return unitIdString; }
    unsigned int index() const { return controllerIndex; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    static inline const std::string typeName{"IDN"};

    std::string unitIdString;
    unsigned int controllerIndex = 0;
    int firmwareVersion = 0;
};

} // namespace libera::idn
