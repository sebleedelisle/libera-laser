#include "libera/lasercubenet/LaserCubeNetControllerInfo.hpp"

namespace libera::lasercubenet {

const std::string LaserCubeNetControllerInfo::typeName{"LaserCubeNet"};

LaserCubeNetControllerInfo::LaserCubeNetControllerInfo(const LaserCubeNetStatus& status)
    : core::DacInfo(status.serialNumber, status.modelName.empty() ? status.serialNumber : status.modelName,
                    status.pointRateMax),
      ip(status.ipAddress),
      serialNumber(status.serialNumber),
      cachedStatus(status) {}

} // namespace libera::lasercubenet
