#pragma once

#include <cstdint>

namespace libera::lasercubeusb {

struct LaserCubeUsbConfig {
    static constexpr std::uint16_t VENDOR_ID = 0x1fc9;
    static constexpr std::uint16_t PRODUCT_ID = 0x04d8;
    static constexpr int BUFFER_CAPACITY = 1024;
};

} // namespace libera::lasercubeusb
