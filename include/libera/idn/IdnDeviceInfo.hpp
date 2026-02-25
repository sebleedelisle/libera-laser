#pragma once

#include "libera/core/GlobalDacManager.hpp"

#include <string>

namespace libera::idn {

class IdnDeviceInfo : public core::DacInfo {
public:
    IdnDeviceInfo(std::string id,
                  std::string label,
                  std::uint32_t maxPointRateValue,
                  unsigned int deviceIndexValue,
                  int firmwareVersionValue)
    : DacInfo(std::move(id), std::move(label), maxPointRateValue)
    , deviceIndex(deviceIndexValue)
    , firmwareVersion(firmwareVersionValue) {}

    const std::string& type() const override { return typeName; }

    unsigned int index() const { return deviceIndex; }
    int firmwareVersionValue() const { return firmwareVersion; }

private:
    static inline const std::string typeName{"idn"};

    unsigned int deviceIndex = 0;
    int firmwareVersion = 0;
};

} // namespace libera::idn
