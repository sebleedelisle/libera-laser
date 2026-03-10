#include "libera/net/TcpClient.hpp"
#include "libera/net/NetConfig.hpp"
#include "libera/etherdream/EtherDreamCommand.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/core/LaserPoint.hpp"

#ifdef _WIN32
#include <cstdio>

int main() {
    std::puts("Skipping EtherDream latency test on Windows.");
    return 0;
}

#else

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace std::chrono_literals;

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "ASSERT TRUE FAILED: %s @ %s:%d\n", msg, __FILE__, __LINE__); \
            std::exit(1); \
        } \
    } while (0)

namespace {

constexpr const char* ETHER_DREAM_IP = "192.168.1.76";
constexpr std::uint32_t POINT_RATE = 30000;
constexpr std::uint16_t SHORT_POINT_COUNT = 200;
constexpr std::uint16_t LONG_POINT_COUNT  = 1800;
constexpr std::size_t   BUFFER_CAPACITY  = 1800;

libera::etherdream::EtherDreamResponse g_lastStatus{};

std::chrono::steady_clock::duration
sendCommand(libera::net::TcpClient& client, libera::etherdream::EtherDreamCommand& command) {
    ASSERT_TRUE(command.isReady(), "command ready");
    auto start = std::chrono::steady_clock::now();
    std::printf("TX opcode %c (%zu bytes)\n", command.commandOpcode(), command.size());
    auto ec = client.write_all(command.data(), command.size());
    ASSERT_TRUE(!ec, "write succeeds");
    std::array<std::uint8_t, 22> ack{};
    while (true) {
        ec = client.read_exact(ack.data(), ack.size());
        ASSERT_TRUE(!ec, "ack received");
        const char frameType = static_cast<char>(ack[0]);
        const char echoed    = static_cast<char>(ack[1]);
        if (g_lastStatus.decode(ack.data(), ack.size())) {
            std::printf("RX %c for opcode %c | buffer=%u rate=%u\n",
                        frameType,
                        echoed,
                        static_cast<unsigned>(g_lastStatus.status.bufferFullness),
                        g_lastStatus.status.pointRate);
        } else {
            std::printf("RX %c for opcode %c (unable to decode status)\n",
                        frameType, echoed);
        }
        if (frameType == 'a' && echoed == command.commandOpcode()) {
            break;
        }
        if (frameType == 'I') {
            std::printf("Encountered invalid frame; waiting for buffer space...\n");
            std::this_thread::sleep_for(2ms);
        }
        if (frameType == 'E') {
            ASSERT_TRUE(false, "EtherDream reported fatal error");
        }
    }
    return std::chrono::steady_clock::now() - start;
}

std::chrono::steady_clock::duration
sendDataPoints(libera::net::TcpClient& client, std::uint16_t pointCount) {
    libera::etherdream::EtherDreamCommand command;
    command.setDataCommand(pointCount);
    libera::core::LaserPoint pt{};
    pt.x = 0.0f;
    pt.y = 0.0f;
    pt.r = pt.g = pt.b = 0.0f;
    pt.i = 0.0f;
    for (std::uint16_t i = 0; i < pointCount; ++i) {
        command.addPoint(pt, i == 0);
    }
    return sendCommand(client, command);
}

void waitForDrain(std::uint16_t pointCount) {
    using namespace std::chrono;
    const double seconds = static_cast<double>(pointCount) / static_cast<double>(POINT_RATE);
    const auto micros = static_cast<long long>(seconds * 1'000'000.0);
    std::this_thread::sleep_for(microseconds(micros) + 2ms);
}

void waitForSpace(libera::net::TcpClient& client, std::uint16_t requiredPoints) {
    while (true) {
        libera::etherdream::EtherDreamCommand query;
        query.setSingleByteCommand('?');
        sendCommand(client, query);
        const auto fullness = g_lastStatus.status.bufferFullness;
        const auto freeSpace = (fullness <= BUFFER_CAPACITY) ? (BUFFER_CAPACITY - fullness) : 0;
        if (freeSpace >= requiredPoints) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
}

} // namespace

int main() {
    libera::net::TcpClient client;
    client.setDefaultTimeout(500ms);
    client.setConnectTimeout(500ms);

    auto endpoint = libera::net::tcp::endpoint(
        libera::net::asio::ip::make_address(ETHER_DREAM_IP),
        libera::etherdream::config::ETHERDREAM_DAC_PORT_DEFAULT);
    auto ec = client.connect(endpoint);
    if (ec) {
        std::fprintf(stderr, "Unable to connect to EtherDream at %s (%s)\n",
                     ETHER_DREAM_IP, ec.message().c_str());
        return 0;
    }
    client.setLowLatency();

    libera::etherdream::EtherDreamCommand command;

    command.setSingleByteCommand('?');
    sendCommand(client, command);

    command.setSingleByteCommand('c');
    sendCommand(client, command);

    command.setSingleByteCommand('p');
    sendCommand(client, command);

    command.setPointRateCommand(POINT_RATE);
    sendCommand(client, command);

    sendDataPoints(client, SHORT_POINT_COUNT);
    waitForDrain(SHORT_POINT_COUNT);

    command.setBeginCommand(POINT_RATE);
    sendCommand(client, command);

    waitForSpace(client, LONG_POINT_COUNT);

    constexpr int sampleCount = 200;
    std::vector<double> shortSamples;
    std::vector<double> longSamples;
    shortSamples.reserve(sampleCount);
    longSamples.reserve(sampleCount);

    for (int i = 0; i < sampleCount; ++i) {
        waitForSpace(client, SHORT_POINT_COUNT);
        auto latency = sendDataPoints(client, SHORT_POINT_COUNT);
        std::printf("short packet %d latency %.3f ms\n", i,
                    std::chrono::duration<double, std::milli>(latency).count());
        shortSamples.push_back(std::chrono::duration<double, std::milli>(latency).count());
    }

    waitForSpace(client, LONG_POINT_COUNT);

    for (int i = 0; i < sampleCount; ++i) {
        waitForSpace(client, LONG_POINT_COUNT);
        auto latency = sendDataPoints(client, LONG_POINT_COUNT);
        std::printf("long  packet %d latency %.3f ms\n", i,
                    std::chrono::duration<double, std::milli>(latency).count());
        longSamples.push_back(std::chrono::duration<double, std::milli>(latency).count());
    }

    auto avg = [](const std::vector<double>& samples) {
        double sum = 0.0;
        for (double v : samples) sum += v;
        return sum / samples.size();
    };

    auto shortLatency = avg(shortSamples);
    auto longLatency = avg(longSamples);

    ASSERT_TRUE(longLatency > shortLatency, "long packets should take longer to ACK on average");

    command.setSingleByteCommand('s');
    sendCommand(client, command);

    client.close();
    std::printf("Short packet latency: %.3f ms\n",
                std::chrono::duration<double, std::milli>(shortLatency).count());
    std::printf("Long  packet latency: %.3f ms\n",
                std::chrono::duration<double, std::milli>(longLatency).count());
    std::puts("EtherDream packet latency test passed.");
    return 0;
}

#endif // _WIN32
