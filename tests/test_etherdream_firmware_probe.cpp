#include "libera/etherdream/EtherDreamController.hpp"

#ifdef _WIN32
#include <cstdio>

int main() {
    std::puts("Skipping EtherDream firmware probe test on Windows.");
    return 0;
}

#else

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "ASSERT TRUE FAILED: %s @ %s:%d\n", msg, __FILE__, __LINE__); \
            std::exit(1); \
        } \
    } while (0)

namespace {

void putLe16(std::array<std::uint8_t, 22>& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xffu);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
}

void putLe32(std::array<std::uint8_t, 22>& data, std::size_t offset, std::uint32_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xffu);
    data[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
    data[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
    data[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
}

bool readExact(int fd, void* dst, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(dst);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t received = ::recv(fd, bytes + offset, size - offset, 0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

bool sendAck(int fd, char command, libera::etherdream::PlaybackState playbackState) {
    std::array<std::uint8_t, 22> data{};
    data[0] = static_cast<std::uint8_t>('a');
    data[1] = static_cast<std::uint8_t>(command);
    data[2] = 0; // protocol version
    data[3] = static_cast<std::uint8_t>(libera::etherdream::LightEngineState::Ready);
    data[4] = static_cast<std::uint8_t>(playbackState);
    data[5] = 0; // network source
    putLe16(data, 6, 0); // light engine flags
    putLe16(data, 8, 0); // playback flags
    putLe16(data, 10, 0); // source flags
    putLe16(data, 12, 0); // buffer fullness
    putLe32(data, 14, 30000); // point rate
    putLe32(data, 18, 0); // point count
    return ::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size());
}

class FirmwareProbeServer {
public:
    FirmwareProbeServer() {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_TRUE(listenFd >= 0, "socket");

        int opt = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        ASSERT_TRUE(::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        ASSERT_TRUE(::listen(listenFd, 1) == 0, "listen");

        socklen_t len = sizeof(addr);
        ASSERT_TRUE(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
        portNumber = ntohs(addr.sin_port);

        running.store(true);
        serverThread = std::thread([this] { run(); });
    }

    ~FirmwareProbeServer() {
        stop();
    }

    unsigned short port() const {
        return portNumber;
    }

    bool firmwareQuerySeen() const {
        return sawFirmwareQuery.load();
    }

    void stop() {
        if (!running.exchange(false)) {
            return;
        }
        if (clientFd >= 0) {
            ::shutdown(clientFd, SHUT_RDWR);
            ::close(clientFd);
            clientFd = -1;
        }
        if (listenFd >= 0) {
            ::shutdown(listenFd, SHUT_RDWR);
            ::close(listenFd);
            listenFd = -1;
        }
        if (serverThread.joinable()) {
            serverThread.join();
        }
    }

private:
    void run() {
        clientFd = ::accept(listenFd, nullptr, nullptr);
        if (clientFd < 0) {
            return;
        }

        if (!sendAck(clientFd, '?', libera::etherdream::PlaybackState::Idle)) {
            return;
        }

        while (running.load()) {
            char command = 0;
            const ssize_t received = ::recv(clientFd, &command, 1, 0);
            if (received <= 0) {
                return;
            }

            if (command == 'v') {
                sawFirmwareQuery.store(true);
                std::array<char, 32> firmware{};
                const std::string value = "Ether Dream test firmware";
                std::memcpy(firmware.data(), value.data(), value.size());
                ::send(clientFd, firmware.data(), firmware.size(), 0);
                continue;
            }

            if (command == 'b') {
                std::array<std::uint8_t, 6> rest{};
                if (!readExact(clientFd, rest.data(), rest.size())) {
                    return;
                }
                if (!sendAck(clientFd, 'b', libera::etherdream::PlaybackState::Playing)) {
                    return;
                }
                continue;
            }

            if (command == 'q') {
                std::array<std::uint8_t, 4> rest{};
                if (!readExact(clientFd, rest.data(), rest.size())) {
                    return;
                }
                if (!sendAck(clientFd, 'q', libera::etherdream::PlaybackState::Playing)) {
                    return;
                }
                continue;
            }

            if (command == 'd') {
                std::array<std::uint8_t, 2> countBytes{};
                if (!readExact(clientFd, countBytes.data(), countBytes.size())) {
                    return;
                }
                const auto count = static_cast<std::uint16_t>(countBytes[0])
                    | static_cast<std::uint16_t>(countBytes[1] << 8);
                std::array<std::uint8_t, 18> point{};
                for (std::uint16_t i = 0; i < count; ++i) {
                    if (!readExact(clientFd, point.data(), point.size())) {
                        return;
                    }
                }
                if (!sendAck(clientFd, 'd', libera::etherdream::PlaybackState::Playing)) {
                    return;
                }
                continue;
            }

            const auto playbackState = command == 'p'
                ? libera::etherdream::PlaybackState::Prepared
                : libera::etherdream::PlaybackState::Idle;
            if (!sendAck(clientFd, command, playbackState)) {
                return;
            }
        }
    }

    int listenFd = -1;
    int clientFd = -1;
    unsigned short portNumber = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> sawFirmwareQuery{false};
    std::thread serverThread;
};

} // namespace

int main() {
    FirmwareProbeServer server;

    libera::etherdream::EtherDreamController controller;
    libera::etherdream::EtherDreamControllerInfo info{
        "loopback",
        "Loopback",
        "127.0.0.1",
        server.port(),
        4096,
        "hw10-sw2",
        30000,
        10,
        2};

    auto connected = controller.connect(info);
    ASSERT_TRUE(connected, "connect should succeed");
    controller.startThread();

    bool sawFirmware = false;
    for (int i = 0; i < 100; ++i) {
        if (server.firmwareQuerySeen()
            && controller.firmwareVersion() == "Ether Dream test firmware") {
            sawFirmware = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }

    controller.stopThread();
    controller.close();
    server.stop();

    ASSERT_TRUE(sawFirmware, "controller should read firmware string with v query");

    std::puts("EtherDream firmware probe test passed.");
    return 0;
}

#endif // _WIN32
