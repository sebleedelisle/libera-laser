#pragma once

#include "libera/core/GlobalDacManager.hpp"

#include <string>

namespace libera::etherdream {

class EtherDreamControllerInfo : public core::DacInfo {
public:
    EtherDreamControllerInfo(std::string id,
                         std::string label,
                         std::string ip,
                         unsigned short port,
                         int bufferSizePoints = 0,
                         std::string hardwareVersion = {},
                         std::uint32_t maxPointRateValue = 0)
    : DacInfo(std::move(id), std::move(label), maxPointRateValue)
    , ipAddress(std::move(ip))
    , portNumber(port)
    , bufferSize(bufferSizePoints)
    , hardwareVersionLabel(std::move(hardwareVersion)) {}

    const std::string& type() const override { return typeName; }

    const std::string& ip() const { return ipAddress; }
    unsigned short port() const { return portNumber; }

    int bufferSizeValue() const { return bufferSize; }
    void setBufferSize(int size) { bufferSize = size; }

    const std::string& hardwareVersion() const { return hardwareVersionLabel; }
    void setHardwareVersion(std::string version) { hardwareVersionLabel = std::move(version); }

private:
    static inline const std::string typeName{"etherdream"};

    std::string ipAddress;
    unsigned short portNumber = 0;
    int bufferSize = 0;
    std::string hardwareVersionLabel;
};

} // namespace libera::etherdream
