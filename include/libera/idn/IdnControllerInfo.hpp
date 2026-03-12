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
                  int firmwareVersionValue)
    : DacInfo(std::move(id), std::move(label), maxPointRateValue)
    , controllerIndex(controllerIndexValue)
    , firmwareVersion(firmwareVersionValue) {}

    const std::string& type() const override { return typeName; }

    unsigned int index() const { return controllerIndex; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    static inline const std::string typeName{"idn"};

    unsigned int controllerIndex = 0;
    int firmwareVersion = 0;
};

} // namespace libera::idn
