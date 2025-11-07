#pragma once

#include "libera/core/DacDiscovery.hpp"

#include <string>

namespace libera::etherdream {

class EtherDreamDeviceInfo : public core::DiscoveredDac {
public:
    EtherDreamDeviceInfo(std::string id,
                         std::string label,
                         std::string ip,
                         unsigned short port)
    : DiscoveredDac(std::move(id), std::move(label))
    , ipAddress(std::move(ip))
    , portNumber(port) {}

    const std::string& type() const override { return typeName; }

    const std::string& ip() const { return ipAddress; }
    unsigned short port() const { return portNumber; }

private:
    static inline const std::string typeName{"etherdream"};

    std::string ipAddress;
    unsigned short portNumber = 0;
};

} // namespace libera::etherdream
