#include "libera/core/LaserPoint.hpp"
#include "libera/etherdream/EtherDreamCommand.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamControllerInfo.hpp"
#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/net/TcpClient.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t POINT_RATE = 30000;
constexpr std::uint16_t STARTUP_POINTS = 200;
constexpr std::uint16_t PROBE_DATA_POINTS = 200;

struct ResponseResult {
    libera::etherdream::EtherDreamResponse response;
    std::error_code error;
};

const char* playbackFlagSummary(const libera::etherdream::EtherDreamStatus& status) {
    if (status.hasPlaybackUnderflow()) {
        return "UNDERFLOW";
    }
    if (status.hasPlaybackEstop()) {
        return "PLAYBACK_ESTOP";
    }
    return "none";
}

void printResponse(const char* label,
                   const libera::etherdream::EtherDreamResponse& response,
                   std::chrono::steady_clock::duration elapsed) {
    std::printf("%-18s RX %c %c in %.3f ms | %s | playback_flags=%s\n",
                label,
                static_cast<char>(response.response),
                static_cast<char>(response.command),
                std::chrono::duration<double, std::milli>(elapsed).count(),
                response.status.describe().c_str(),
                playbackFlagSummary(response.status));
}

ResponseResult sendCommand(libera::net::TcpClient& client,
                           const char* label,
                           libera::etherdream::EtherDreamCommand& command) {
    ResponseResult result;

    const char expectedCommand = command.commandOpcode();
    const auto start = std::chrono::steady_clock::now();
    auto ec = client.write_all(command.data(), command.size(), 500ms);
    if (ec) {
        result.error = ec;
        std::fprintf(stderr, "%-18s write failed: %s\n", label, ec.message().c_str());
        return result;
    }

    for (int attempt = 0; attempt < 10; ++attempt) {
        std::array<std::uint8_t, 22> ack{};
        ec = client.read_exact(ack.data(), ack.size(), 500ms);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (ec) {
            result.error = ec;
            std::fprintf(stderr, "%-18s read failed: %s\n", label, ec.message().c_str());
            return result;
        }

        if (!result.response.decode(ack.data(), ack.size())) {
            result.error = std::make_error_code(std::errc::protocol_error);
            std::fprintf(stderr, "%-18s decode failed\n", label);
            return result;
        }

        printResponse(label, result.response, elapsed);
        if (static_cast<char>(result.response.command) == expectedCommand) {
            return result;
        }

        std::printf("%-18s ignoring unmatched response for command %c while waiting for %c\n",
                    label,
                    static_cast<char>(result.response.command),
                    expectedCommand);
    }

    result.error = std::make_error_code(std::errc::protocol_error);
    std::fprintf(stderr, "%-18s did not receive a matching response for %c\n",
                 label,
                 expectedCommand);
    return result;
}

void readInitialStatusIfPresent(libera::net::TcpClient& client) {
    std::array<std::uint8_t, 22> ack{};
    std::size_t bytesTransferred = 0;
    const auto start = std::chrono::steady_clock::now();
    auto ec = client.read_exact(ack.data(), ack.size(), 200ms, &bytesTransferred);
    if (ec == libera::net::asio::error::timed_out) {
        std::puts("No initial status frame arrived before the first command.");
        return;
    }
    if (ec) {
        std::fprintf(stderr, "initial-status   read failed: %s bytes=%zu\n",
                     ec.message().c_str(),
                     bytesTransferred);
        return;
    }

    libera::etherdream::EtherDreamResponse response;
    if (!response.decode(ack.data(), ack.size())) {
        std::fprintf(stderr, "initial-status   decode failed\n");
        return;
    }

    printResponse("initial-status", response, std::chrono::steady_clock::now() - start);
}

bool sendSingleByte(libera::net::TcpClient& client, const char* label, char opcode) {
    libera::etherdream::EtherDreamCommand command;
    command.setSingleByteCommand(opcode);
    const auto result = sendCommand(client, label, command);
    return !result.error;
}

bool sendBegin(libera::net::TcpClient& client) {
    libera::etherdream::EtherDreamCommand command;
    command.setBeginCommand(POINT_RATE);
    const auto result = sendCommand(client, "begin", command);
    return !result.error;
}

bool sendPointRate(libera::net::TcpClient& client) {
    libera::etherdream::EtherDreamCommand command;
    command.setPointRateCommand(POINT_RATE);
    const auto result = sendCommand(client, "point-rate", command);
    return !result.error;
}

ResponseResult sendBlankData(libera::net::TcpClient& client,
                             const char* label,
                             std::uint16_t pointCount) {
    libera::etherdream::EtherDreamCommand command;
    command.setDataCommand(pointCount);

    libera::core::LaserPoint point{};
    point.x = 0.0f;
    point.y = 0.0f;
    point.r = 0.0f;
    point.g = 0.0f;
    point.b = 0.0f;
    point.i = 0.0f;
    point.u1 = 0.0f;
    point.u2 = 0.0f;

    for (std::uint16_t i = 0; i < pointCount; ++i) {
        command.addPoint(point, i == 0);
    }

    return sendCommand(client, label, command);
}

std::string discoverEtherDreamIp() {
    libera::etherdream::EtherDreamManager manager;
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto discovered = manager.discover();
        for (const auto& info : discovered) {
            const auto* etherDream =
                dynamic_cast<const libera::etherdream::EtherDreamControllerInfo*>(info.get());
            if (!etherDream) {
                continue;
            }

            std::printf("Discovered %s at %s:%u buffer=%d max_rate=%u\n",
                        etherDream->labelValue().c_str(),
                        etherDream->ip().c_str(),
                        etherDream->port(),
                        etherDream->bufferSizeValue(),
                        etherDream->maxPointRate());
            return etherDream->ip();
        }
        std::this_thread::sleep_for(250ms);
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    std::string ip;
    if (argc >= 2) {
        ip = argv[1];
    } else {
        ip = discoverEtherDreamIp();
    }

    if (ip.empty()) {
        std::fprintf(stderr, "No Ether Dream discovered. Pass an IP as argv[1].\n");
        return 2;
    }

    libera::net::TcpClient client;
    client.setConnectTimeout(1000ms);
    client.setDefaultTimeout(500ms);

    auto endpoint = libera::net::tcp::endpoint(
        libera::net::asio::ip::make_address(ip),
        libera::etherdream::config::ETHERDREAM_DAC_PORT_DEFAULT);

    std::printf("Connecting to Ether Dream at %s:%u\n",
                ip.c_str(),
                libera::etherdream::config::ETHERDREAM_DAC_PORT_DEFAULT);
    auto ec = client.connect(endpoint);
    if (ec) {
        std::fprintf(stderr, "Connect failed: %s\n", ec.message().c_str());
        return 1;
    }
    client.setLowLatency();
    readInitialStatusIfPresent(client);

    bool ok = true;
    ok = ok && sendSingleByte(client, "query-initial", '?');
    ok = ok && sendSingleByte(client, "stop", 's');
    ok = ok && sendSingleByte(client, "clear", 'c');
    ok = ok && sendSingleByte(client, "prepare", 'p');
    ok = ok && sendPointRate(client);
    ok = ok && !sendBlankData(client, "data-start", STARTUP_POINTS).error;
    ok = ok && sendBegin(client);

    if (!ok) {
        sendSingleByte(client, "stop-cleanup", 's');
        client.close();
        return 1;
    }

    const auto drainTime = std::chrono::microseconds(
        static_cast<long long>((static_cast<double>(STARTUP_POINTS) /
                                static_cast<double>(POINT_RATE)) * 1'000'000.0));
    std::printf("Waiting %.3f ms for the %u blank startup points to drain, plus 150 ms starvation.\n",
                std::chrono::duration<double, std::milli>(drainTime).count(),
                STARTUP_POINTS);
    std::this_thread::sleep_for(drainTime + 150ms);

    for (int i = 0; i < 8; ++i) {
        sendSingleByte(client, "query-starved", '?');
        std::this_thread::sleep_for(25ms);
    }

    std::puts("Sending one blank data packet after starvation to capture the DAC response.");
    sendBlankData(client, "data-after-idle", PROBE_DATA_POINTS);

    for (int i = 0; i < 4; ++i) {
        sendSingleByte(client, "query-after-data", '?');
        std::this_thread::sleep_for(25ms);
    }

    sendSingleByte(client, "stop-cleanup", 's');
    client.close();
    return 0;
}
