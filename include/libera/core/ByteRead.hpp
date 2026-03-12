#pragma once

#include <cstdint>

namespace libera::core::bytes {

// Read little-endian values (least-significant byte first) from raw bytes.
// These helpers remove repeated byte-shift code in packet parsers.
inline std::uint16_t readLe16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}

inline std::uint32_t readLe32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

} // namespace libera::core::bytes
