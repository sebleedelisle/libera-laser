#include "libera/core/LaserPoint.hpp"
#include "libera/lightspacenet/LightSpaceNetConfig.hpp"
#include "libera/lightspacenet/LightSpaceNetPacket.hpp"
#include "libera/lightspacenet/LightSpaceNetStatus.hpp"
#include "libera/log/Log.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/net/UdpSocket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace libera;
using namespace libera::core;
using namespace libera::lightspacenet;
using namespace std::chrono_literals;

namespace {

constexpr std::array<std::uint8_t, 10> protocolHeader{
    'L', 'I', 'G', 'H', 'T', 'S', 'P', 'A', 'C', 'E'
};

struct Options {
    std::string targetIp;
    std::string discoveryTarget = "255.255.255.255";
    std::vector<std::size_t> pointCounts{700, 728, 729};
};

struct ReadPacketResult {
    std::optional<LightSpaceNetPacket> packet;
    std::error_code error;
    std::string detail;
    std::size_t bytesRead = 0;
};

std::uint64_t steadyMillis() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::vector<LaserPoint> makeBlankPoints(std::size_t pointCount) {
    std::vector<LaserPoint> points(pointCount);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float t = points.size() > 1
            ? static_cast<float>(i) / static_cast<float>(points.size() - 1)
            : 0.5f;
        points[i].x = -0.5f + t;
        points[i].y = 0.0f;
        points[i].r = 0.0f;
        points[i].g = 0.0f;
        points[i].b = 0.0f;
        points[i].i = 0.0f;
    }
    return points;
}

std::optional<std::size_t> parsePointCount(const std::string& value) {
    char* end = nullptr;
    const auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed == 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

std::optional<std::vector<std::size_t>> parsePointCounts(const std::string& value) {
    std::vector<std::size_t> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        const auto item = value.substr(start, comma == std::string::npos
                                                  ? std::string::npos
                                                  : comma - start);
        auto parsed = parsePointCount(item);
        if (!parsed) {
            return std::nullopt;
        }
        result.push_back(*parsed);
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return result;
}

void printUsage(const char* executableName) {
    logInfo("Usage:", executableName, "[device-ip] [--discover=target] [--points=700,728,729]");
    logInfo("If device-ip is omitted, the probe uses LS-Net UDP discovery first.");
    logInfo("The point packets are blank; the probe does not switch laser output on.");
}

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return std::nullopt;
        }
        if (arg.rfind("--discover=", 0) == 0) {
            options.discoveryTarget = arg.substr(std::string("--discover=").size());
            continue;
        }
        if (arg.rfind("--points=", 0) == 0) {
            auto counts = parsePointCounts(arg.substr(std::string("--points=").size()));
            if (!counts) {
                logError("Invalid --points list:", arg);
                return std::nullopt;
            }
            options.pointCounts = *counts;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            logError("Unknown option:", arg);
            printUsage(argv[0]);
            return std::nullopt;
        }
        if (!options.targetIp.empty()) {
            logError("Only one device IP may be supplied.");
            printUsage(argv[0]);
            return std::nullopt;
        }
        options.targetIp = arg;
    }
    return options;
}

std::optional<std::string> discoverDeviceIp(const std::string& target) {
    std::error_code addressError;
    const auto address = net::asio::ip::make_address(target, addressError);
    if (addressError) {
        logError("Invalid discovery target:", target, addressError.message());
        return std::nullopt;
    }

    auto io = net::shared_io_context();
    net::UdpSocket socket(*io);
    if (auto ec = socket.open_v4()) {
        logError("UDP open failed:", ec.message());
        return std::nullopt;
    }
    socket.enable_broadcast(true);
    if (auto ec = socket.bind_any(0, false)) {
        logError("UDP bind failed:", ec.message());
        return std::nullopt;
    }

    const auto query = buildBroadcastQueryPacket();
    const net::udp::endpoint targetEndpoint(address, LightSpaceNetConfig::NETWORK_PORT);
    logInfo("Discovering LS-Net controller via", targetEndpoint.address().to_string(),
            targetEndpoint.port());
    if (auto ec = socket.send_to(query.data(), query.size(), targetEndpoint, 200ms)) {
        logError("Discovery send failed:", ec.message());
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<std::uint8_t, 2048> buffer{};
        net::udp::endpoint sender;
        std::size_t received = 0;
        auto ec = socket.recv_from(buffer.data(), buffer.size(), sender, received, 250ms, false);
        if (ec == net::asio::error::timed_out ||
            ec == net::asio::error::operation_aborted) {
            continue;
        }
        if (ec) {
            logError("Discovery receive failed:", ec.message());
            return std::nullopt;
        }
        if (received == query.size() &&
            std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received),
                       query.begin())) {
            continue;
        }

        if (auto status = LightSpaceNetStatus::parseBroadcastResponse(buffer.data(), received)) {
            logInfo("Discovered", status->displayLabel(), "at", status->ipAddress,
                    "sender", sender.address().to_string());
            return status->ipAddress.empty()
                ? std::optional<std::string>(sender.address().to_string())
                : std::optional<std::string>(status->ipAddress);
        }

        logInfo("Ignoring non-LS-Net UDP reply from", sender.address().to_string(),
                sender.port(), "bytes", received);
    }

    return std::nullopt;
}

std::error_code connectClient(net::TcpClient& client, const std::string& ip) {
    std::error_code addressError;
    const auto address = net::asio::ip::make_address(ip, addressError);
    if (addressError) {
        return addressError;
    }

    client.setConnectTimeout(1000ms);
    client.setDefaultTimeout(500ms);
    const net::tcp::endpoint endpoint(address, LightSpaceNetConfig::NETWORK_PORT);
    auto ec = client.connect(endpoint, 1000ms);
    if (!ec) {
        client.setLowLatency();
    }
    return ec;
}

ReadPacketResult readPacket(net::TcpClient& client, std::chrono::milliseconds timeout) {
    ReadPacketResult result;
    std::array<std::uint8_t, 12> prefix{};
    std::size_t bytes = 0;
    result.error = client.read_exact(prefix.data(), prefix.size(), timeout, &bytes);
    result.bytesRead += bytes;
    if (result.error) {
        result.detail = "failed while reading packet prefix";
        return result;
    }

    if (!std::equal(protocolHeader.begin(), protocolHeader.end(), prefix.begin())) {
        result.detail = "packet header did not match LIGHTSPACE";
        return result;
    }

    const auto packetSize = static_cast<std::size_t>(readBe16(prefix.data() + 10));
    if (packetSize < 20) {
        result.detail = "packet length was below protocol minimum";
        return result;
    }

    std::vector<std::uint8_t> raw(packetSize);
    std::copy(prefix.begin(), prefix.end(), raw.begin());
    result.error = client.read_exact(raw.data() + prefix.size(),
                                     raw.size() - prefix.size(),
                                     timeout,
                                     &bytes);
    result.bytesRead += bytes;
    if (result.error) {
        result.detail = "failed while reading packet body";
        return result;
    }

    result.packet = parsePacket(raw.data(), raw.size());
    if (!result.packet) {
        result.detail = "response failed LS-Net packet parsing";
    }
    return result;
}

bool sendHeartbeatAndReadResponse(net::TcpClient& client,
                                  const std::string& label,
                                  std::chrono::milliseconds timeout) {
    const auto heartbeat = buildHeartbeatQueryPacket(steadyMillis());
    if (auto ec = client.write_all(heartbeat.data(), heartbeat.size(), timeout)) {
        logError(label, "heartbeat send failed:", ec.message());
        return false;
    }

    auto response = readPacket(client, timeout);
    if (response.error) {
        logError(label, "heartbeat response failed:", response.error.message(),
                 response.detail, "bytes", response.bytesRead);
        return false;
    }
    if (!response.packet) {
        logError(label, "heartbeat response invalid:", response.detail,
                 "bytes", response.bytesRead);
        return false;
    }
    if (response.packet->packetType != LightSpaceNetConfig::PACKET_TYPE_BASIC ||
        response.packet->commandWord != LightSpaceNetConfig::CMD_HEARTBEAT_RESPONSE) {
        logError(label, "unexpected response packet type",
                 static_cast<int>(response.packet->packetType),
                 "command",
                 static_cast<int>(response.packet->commandWord));
        return false;
    }

    logInfo(label, "heartbeat OK");
    return true;
}

bool confirmFreshSession(const std::string& ip) {
    std::this_thread::sleep_for(500ms);
    net::TcpClient client;
    if (auto ec = connectClient(client, ip)) {
        logError("fresh session connect failed:", ec.message());
        return false;
    }
    const bool ok = sendHeartbeatAndReadResponse(client, "fresh session", 500ms);
    client.close();
    return ok;
}

bool probePointCount(const std::string& ip, std::size_t pointCount) {
    net::TcpClient client;
    if (auto ec = connectClient(client, ip)) {
        logError("connect failed for", pointCount, "points:", ec.message());
        return false;
    }

    if (!sendHeartbeatAndReadResponse(client, "preflight " + std::to_string(pointCount), 500ms)) {
        client.close();
        return false;
    }

    const auto packet = buildPointStreamPacket(makeBlankPoints(pointCount));
    if (packet.empty()) {
        logError("failed to build point packet for", pointCount, "points");
        client.close();
        return false;
    }

    logInfo("Sending", pointCount, "blank points as", packet.size(), "bytes");
    if (auto ec = client.write_all(packet.data(), packet.size(), 500ms)) {
        logError(pointCount, "point packet send failed:", ec.message());
        client.close();
        return false;
    }

    // Give the firmware a short chance to parse the point stream before asking
    // for a heartbeat. A responsive device should still answer on this session.
    std::this_thread::sleep_for(100ms);
    const bool responsive =
        sendHeartbeatAndReadResponse(client, "post-frame " + std::to_string(pointCount), 700ms);
    client.close();
    if (!responsive) {
        logError(pointCount, "points left the current TCP session unresponsive");
    }
    return responsive;
}

} // namespace

int main(int argc, char** argv) {
    auto parsedOptions = parseOptions(argc, argv);
    if (!parsedOptions) {
        return argc > 1 && (std::string(argv[1]) == "--help" ||
                            std::string(argv[1]) == "-h")
            ? 0
            : 1;
    }

    Options options = *parsedOptions;
    if (options.targetIp.empty()) {
        auto discoveredIp = discoverDeviceIp(options.discoveryTarget);
        if (!discoveredIp) {
            logError("No LS-Net controller discovered.");
            return 1;
        }
        options.targetIp = *discoveredIp;
    }

    logInfo("=== LS-Net packet limit probe ===");
    logInfo("Target", options.targetIp, "TCP port", LightSpaceNetConfig::NETWORK_PORT);
    logInfo("Point packet size is 22 + (points * 7) bytes.");

    bool allResponsive = true;
    for (const auto pointCount : options.pointCounts) {
        const bool responsive = probePointCount(options.targetIp, pointCount);
        allResponsive = allResponsive && responsive;
        if (!responsive) {
            (void)confirmFreshSession(options.targetIp);
        }
    }

    if (!allResponsive) {
        logError("One or more packet sizes made the current TCP session unresponsive.");
        return 2;
    }

    logInfo("All probed packet sizes remained responsive.");
    return 0;
}
