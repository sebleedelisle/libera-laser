#include "libera/lightspacenet/LightSpaceNetConfig.hpp"
#include "libera/lightspacenet/LightSpaceNetPacket.hpp"
#include "libera/lightspacenet/LightSpaceNetStatus.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/log/Log.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

using namespace libera;
using namespace libera::lightspacenet;

namespace {

std::string hexBytes(const std::uint8_t* data, std::size_t size) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        if (i > 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<int>(data[i]);
    }
    return stream.str();
}

int runProbe(const std::string& modeName,
             std::uint16_t localPort,
             const net::udp::endpoint& targetEndpoint) {
    auto io = net::shared_io_context();
    net::UdpSocket socket(*io);
    if (auto ec = socket.open_v4()) {
        logError("open failed", ec.message());
        return 0;
    }
    socket.enable_broadcast(true);
    if (auto ec = socket.bind_any(localPort, false)) {
        logError(modeName, "bind failed on local port", localPort, ec.message());
        return 0;
    }

    const auto packet = buildBroadcastQueryPacket();
    logInfo(modeName,
            "sending query from local port",
            localPort,
            "to",
            targetEndpoint.address().to_string(),
            targetEndpoint.port());
    if (auto ec = socket.send_to(packet.data(), packet.size(), targetEndpoint,
                                 std::chrono::milliseconds(200))) {
        logError(modeName, "send failed", ec.message());
        return 0;
    }

    int replies = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<std::uint8_t, 2048> buffer{};
        net::udp::endpoint sender;
        std::size_t received = 0;
        auto ec = socket.recv_from(buffer.data(), buffer.size(), sender, received,
                                   std::chrono::milliseconds(250), false);
        if (ec == net::asio::error::timed_out ||
            ec == net::asio::error::operation_aborted) {
            continue;
        }
        if (ec) {
            logError(modeName, "receive failed", ec.message());
            break;
        }

        if (received == packet.size() &&
            std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(received),
                       packet.begin())) {
            logInfo(modeName,
                    "ignored echoed query from",
                    sender.address().to_string(),
                    sender.port());
            continue;
        }

        ++replies;
        logInfo(modeName,
                "reply from",
                sender.address().to_string(),
                sender.port(),
                "bytes",
                received);
        logInfo("  raw:", hexBytes(buffer.data(), received));

        if (auto status = LightSpaceNetStatus::parseBroadcastResponse(buffer.data(), received)) {
            logInfo("  parsed:",
                    "name", status->displayLabel(),
                    "id", status->stableId(),
                    "payload_ip", status->ipAddress,
                    "mac", status->macAddressString,
                    "fw", status->firmwareVersion,
                    "hw", status->hardwareVersion);
        }
    }

    socket.close();
    return replies;
}

} // namespace

int main(int argc, char** argv) {
    const std::string target = argc > 1 ? argv[1] : "255.255.255.255";
    std::error_code ec;
    auto address = net::asio::ip::make_address(target, ec);
    if (ec) {
        logError("Invalid target address:", target, ec.message());
        return 1;
    }

    const net::udp::endpoint targetEndpoint(address, LightSpaceNetConfig::NETWORK_PORT);
    logInfo("LS-Net probe target:", targetEndpoint.address().to_string(), targetEndpoint.port());
    logInfo("Tip: if 255.255.255.255 gets no replies, try your subnet broadcast, e.g. 192.168.1.255, or the device IP.");

    int totalReplies = 0;
    totalReplies += runProbe("documented-port",
                             LightSpaceNetConfig::NETWORK_PORT,
                             targetEndpoint);
    totalReplies += runProbe("ephemeral-port", 0, targetEndpoint);

    if (totalReplies == 0) {
        logError("No LS-Net replies received.");
        return 1;
    }

    logInfo("Total LS-Net replies:", totalReplies);
    return 0;
}
