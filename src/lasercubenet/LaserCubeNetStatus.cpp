#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/core/ByteRead.hpp"

#include <sstream>
#include <iomanip>
#include <cstring>

namespace libera::lasercubenet {
namespace {
inline std::string hex_serial(const std::uint8_t* ptr, std::size_t len) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex;
    for (std::size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << std::setfill('0') << static_cast<int>(ptr[i]);
    }
    return oss.str();
}
}

std::optional<LaserCubeNetStatus> LaserCubeNetStatus::parse(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 64) {
        return std::nullopt;
    }

    LaserCubeNetStatus status;
    status.payloadVersion = data[2];
    if (status.payloadVersion != 0) {
        return std::nullopt; // unknown payload revision
    }

    status.firmwareMajor = data[3];
    status.firmwareMinor = data[4];
    status.firmwareVersion = std::to_string(status.firmwareMajor) + "." + std::to_string(status.firmwareMinor);

    const std::uint8_t flags = data[5];
    status.outputEnabled = (flags & 0x01) != 0;
    const bool newFlagLayout = (status.firmwareMajor > 0) || (status.firmwareMinor >= 13);
    if (newFlagLayout) {
        status.interlockEnabled = (flags & 0x02) != 0;
        status.temperatureWarning = (flags & 0x04) != 0;
        status.overTemperature = (flags & 0x08) != 0;
        status.packetErrors = static_cast<std::uint8_t>((flags >> 4) & 0x0F);
    } else {
        status.interlockEnabled = (flags & 0x08) != 0;
        status.temperatureWarning = (flags & 0x10) != 0;
        status.overTemperature = (flags & 0x20) != 0;
    }

    status.pointRate = core::bytes::readLe32(&data[10]);
    status.pointRateMax = core::bytes::readLe32(&data[14]);
    status.bufferFree = core::bytes::readLe16(&data[19]);
    status.bufferMax = core::bytes::readLe16(&data[21]);
    status.batteryPercent = data[23];
    status.temperatureC = data[24];
    status.connectionType = data[25];

    status.serialNumber = hex_serial(&data[26], 6);
    status.ipAddress = std::to_string(data[32]) + "." +
                       std::to_string(data[33]) + "." +
                       std::to_string(data[34]) + "." +
                       std::to_string(data[35]);

    status.modelNumber = data[37];
    const char* namePtr = reinterpret_cast<const char*>(&data[38]);
    const std::size_t nameLen = strnlen(namePtr, size - 38);
    status.modelName.assign(namePtr, nameLen);
    status.lastSeen = std::chrono::steady_clock::now();
    return status;
}

} // namespace libera::lasercubenet
