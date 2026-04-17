#pragma once

#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/System.hpp"

namespace libera::lasercubenet {

class LaserCubeNetControllerInfo : public core::ControllerInfo {
public:
    LaserCubeNetControllerInfo(const LaserCubeNetStatus& status);

    const std::string& ipAddress() const { return ip; }
    const std::string& serial() const { return serialNumber; }
    const LaserCubeNetStatus& status() const { return cachedStatus; }

private:
    std::string ip;
    std::string serialNumber;
    LaserCubeNetStatus cachedStatus;
};

} // namespace libera::lasercubenet
