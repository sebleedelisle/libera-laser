#include "libera/lasercubenet/LaserCubeNetControllerInfo.hpp"
#include "libera/lasercubenet/LaserCubeNetConfig.hpp"

namespace libera::lasercubenet {

LaserCubeNetControllerInfo::LaserCubeNetControllerInfo(const LaserCubeNetStatus& status)
    : core::ControllerInfo(LaserCubeNetControllerInfo::controllerType(),
                           status.serialNumber,
                           status.modelName.empty() ? status.serialNumber : status.modelName,
                           status.pointRateMax,
                           core::ControllerInfo::NetworkInfo{
                               status.ipAddress,
                               LaserCubeNetConfig::DATA_PORT
                           }),
      ip(status.ipAddress),
      serialNumber(status.serialNumber),
      cachedStatus(status) {}

} // namespace libera::lasercubenet
