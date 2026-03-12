#pragma once

#include "libera/core/GlobalDacManager.hpp"

#include <string>

namespace libera::idn {

class IdnControllerInfo : public core::DacInfo {
public:
    IdnControllerInfo(std::string id,
                  std::string label,
                  std::uint32_t maxPointRateValue,
                  unsigned int controllerIndexValue,
                  int firmwareVersionValue,
                  std::optional<core::DacInfo::NetworkInfo> networkInfo = std::nullopt)
    : DacInfo(std::move(id), std::move(label), maxPointRateValue, std::move(networkInfo))
    , controllerIndex(controllerIndexValue)
    , firmwareVersion(firmwareVersionValue) {}

    const std::string& type() const override { return typeName; }

    unsigned int index() const { return controllerIndex; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    static inline const std::string typeName{"IDN"};

    unsigned int controllerIndex = 0;
    int firmwareVersion = 0;
};

} // namespace libera::idn
