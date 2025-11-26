#pragma once

#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/core/GlobalDacManager.hpp"

namespace libera::lasercubenet {

class LaserCubeNetDeviceInfo : public core::DacInfo {
public:
    LaserCubeNetDeviceInfo(const LaserCubeNetStatus& status);

    const std::string& ipAddress() const { return ip; }
    const std::string& serial() const { return serialNumber; }
    const LaserCubeNetStatus& status() const { return cachedStatus; }

    const std::string& type() const override { return typeName; }

private:
    static const std::string typeName;
    std::string ip;
    std::string serialNumber;
    LaserCubeNetStatus cachedStatus;
};

} // namespace libera::lasercubenet
