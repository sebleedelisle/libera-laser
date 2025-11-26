#include "libera/lasercubenet/LaserCubeNetDeviceInfo.hpp"

namespace libera::lasercubenet {

const std::string LaserCubeNetDeviceInfo::typeName{"lasercube_net"};

LaserCubeNetDeviceInfo::LaserCubeNetDeviceInfo(const LaserCubeNetStatus& status)
    : core::DacInfo(status.serialNumber, status.modelName.empty() ? status.serialNumber : status.modelName,
                    status.pointRateMax),
      ip(status.ipAddress),
      serialNumber(status.serialNumber),
      cachedStatus(status) {}

} // namespace libera::lasercubenet
