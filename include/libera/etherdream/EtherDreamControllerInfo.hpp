#pragma once

#include "libera/System.hpp"

#include <string>

namespace libera::etherdream {

class EtherDreamControllerInfo : public core::ControllerInfo {
public:
    static constexpr std::string_view controllerType() {
        return "EtherDream";
    }

    EtherDreamControllerInfo(std::string id,
                             std::string label,
                             std::string ip,
                             unsigned short port,
                             int bufferSizePoints = 0,
                             std::string hardwareVersion = {},
                             std::uint32_t maxPointRateValue = 0,
                             std::uint16_t hardwareRevision = 0,
                             std::uint16_t softwareRevision = 0,
                             std::string firmwareVersion = {})
    : ControllerInfo(controllerType(),
                     std::move(id),
                     std::move(label),
                     maxPointRateValue,
                     core::ControllerInfo::NetworkInfo{ip, port})
    , ipAddress(std::move(ip))
    , portNumber(port)
    , bufferSize(bufferSizePoints)
    , hardwareVersionLabel(std::move(hardwareVersion))
    , hardwareRevisionValue(hardwareRevision)
    , softwareRevisionValue(softwareRevision)
    , firmwareVersionLabel(std::move(firmwareVersion)) {}

    const std::string& ip() const { return ipAddress; }
    unsigned short port() const { return portNumber; }

    int bufferSizeValue() const { return bufferSize; }
    void setBufferSize(int size) { bufferSize = size; }

    const std::string& hardwareVersion() const { return hardwareVersionLabel; }
    void setHardwareVersion(std::string version) { hardwareVersionLabel = std::move(version); }

    std::uint16_t hardwareRevision() const { return hardwareRevisionValue; }
    void setHardwareRevision(std::uint16_t revision) { hardwareRevisionValue = revision; }

    std::uint16_t softwareRevision() const { return softwareRevisionValue; }
    void setSoftwareRevision(std::uint16_t revision) { softwareRevisionValue = revision; }

    const std::string& firmwareVersion() const { return firmwareVersionLabel; }
    void setFirmwareVersion(std::string version) { firmwareVersionLabel = std::move(version); }

private:
    std::string ipAddress;
    unsigned short portNumber = 0;
    int bufferSize = 0;
    std::string hardwareVersionLabel;
    std::uint16_t hardwareRevisionValue = 0;
    std::uint16_t softwareRevisionValue = 0;
    std::string firmwareVersionLabel;
};

} // namespace libera::etherdream
