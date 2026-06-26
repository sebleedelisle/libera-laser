#include "libera/lightspacenet/LightSpaceNetStatus.hpp"

#include "libera/lightspacenet/LightSpaceNetConfig.hpp"
#include "libera/lightspacenet/LightSpaceNetPacket.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <vector>

namespace libera::lightspacenet {

namespace {

std::string makeHexString(const std::uint8_t* data, std::size_t size) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex;
    for (std::size_t i = 0; i < size; ++i) {
        stream << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return stream.str();
}

std::string makeIpString(const std::uint8_t* data) {
    return std::to_string(data[0]) + "." +
           std::to_string(data[1]) + "." +
           std::to_string(data[2]) + "." +
           std::to_string(data[3]);
}

std::string sanitizeDeviceName(std::string value) {
    while (!value.empty() &&
           (value.back() == '\0' ||
            std::isspace(static_cast<unsigned char>(value.back())) != 0)) {
        value.pop_back();
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    for (char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte == 0 || std::isprint(byte) == 0) {
            return {};
        }
    }
    return value;
}

bool isCommonSubnetMask(const std::uint8_t* data) {
    const auto mask = readBe32(data);
    switch (mask) {
    case 0xFF000000u:
    case 0xFFFF0000u:
    case 0xFFFFFF00u:
    case 0xFFFFFF80u:
    case 0xFFFFFFC0u:
    case 0xFFFFFFE0u:
    case 0xFFFFFFF0u:
    case 0xFFFFFFF8u:
    case 0xFFFFFFFCu:
        return true;
    default:
        return false;
    }
}

bool isLikelyIpv4Address(const std::uint8_t* data) {
    return data[0] != 0 && data[0] != 255 && data[3] != 0 && data[3] != 255;
}

bool isObservedNetworkLayout(const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 19) {
        return false;
    }

    // Some LS-Net firmware replies with:
    // device id, firmware, IP, subnet mask, gateway, scan frequency/hardware byte.
    // That differs from the public PDF, which lists hardware version and MAC.
    return isLikelyIpv4Address(payload.data() + 6) &&
           isCommonSubnetMask(payload.data() + 10) &&
           isLikelyIpv4Address(payload.data() + 14);
}

} // namespace

std::string LightSpaceNetStatus::stableId() const {
    const bool hasMac = std::any_of(macAddress.begin(), macAddress.end(),
                                   [](std::uint8_t value) { return value != 0; });
    if (hasMac) {
        return macAddressString;
    }
    return std::to_string(deviceId);
}

std::string LightSpaceNetStatus::displayLabel() const {
    if (!deviceName.empty()) {
        return deviceName;
    }
    return "LightSpace " + stableId();
}

std::optional<LightSpaceNetStatus> LightSpaceNetStatus::parseBroadcastResponse(
    const std::uint8_t* data,
    std::size_t size) {
    auto packet = parsePacket(data, size);
    if (!packet) {
        return std::nullopt;
    }
    if (packet->packetType != LightSpaceNetConfig::PACKET_TYPE_BASIC ||
        packet->commandWord != LightSpaceNetConfig::CMD_BROADCAST_RESPONSE) {
        return std::nullopt;
    }

    constexpr std::size_t documentedFixedPayloadSize = 18;
    if (packet->payload.size() < documentedFixedPayloadSize) {
        return std::nullopt;
    }

    LightSpaceNetStatus status;
    const auto* payload = packet->payload.data();
    status.deviceId = readBe32(payload);
    status.firmwareVersion = readBe16(payload + 4);

    if (isObservedNetworkLayout(packet->payload)) {
        status.hardwareVersion = payload[18];
        status.ipAddress = makeIpString(payload + 6);
    } else {
        status.hardwareVersion = readBe16(payload + 6);
        status.ipAddress = makeIpString(payload + 8);
        std::copy(payload + 12, payload + 18, status.macAddress.begin());
        status.macAddressString = makeHexString(status.macAddress.data(), status.macAddress.size());

        if (packet->payload.size() > documentedFixedPayloadSize) {
            status.deviceName = sanitizeDeviceName(
                std::string(reinterpret_cast<const char*>(payload + documentedFixedPayloadSize),
                            packet->payload.size() - documentedFixedPayloadSize));
        }
    }

    status.lastSeen = std::chrono::steady_clock::now();
    return status;
}

} // namespace libera::lightspacenet
