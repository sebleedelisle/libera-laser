#include "libera/etherdream/EtherDreamController.hpp"

#ifdef _WIN32
#include <cstdio>

int main() {
    std::puts("Skipping EtherDream begin timeout reconnect test on Windows.");
    return 0;
}

#else

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
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

std::uint16_t readLe16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0])
        | static_cast<std::uint16_t>(data[1] << 8);
}

class BeginTimeoutServer {
public:
    BeginTimeoutServer() {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_TRUE(listenFd >= 0, "socket");

        int opt = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        ASSERT_TRUE(::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        ASSERT_TRUE(::listen(listenFd, 4) == 0, "listen");

        socklen_t len = sizeof(addr);
        ASSERT_TRUE(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
        portNumber = ntohs(addr.sin_port);

        running.store(true);
        serverThread = std::thread([this] { run(); });
    }

    ~BeginTimeoutServer() {
        stop();
    }

    void stop() {
        if (!running.exchange(false)) {
            return;
        }

        const int activeClient = clientFd.exchange(-1);
        if (activeClient >= 0) {
            ::shutdown(activeClient, SHUT_RDWR);
            ::close(activeClient);
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

    unsigned short port() const {
        return portNumber;
    }

    bool waitForRecoveredBegin(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, timeout, [this] {
            return recoveredBeginSeen || !violationMessage.empty();
        }) && recoveredBeginSeen && violationMessage.empty();
    }

    std::string violation() const {
        std::lock_guard<std::mutex> lock(mutex);
        return violationMessage;
    }

    int connectionCount() const {
        return acceptedConnections.load();
    }

private:
    bool readExact(int fd, void* dst, std::size_t size) {
        auto* bytes = static_cast<std::uint8_t*>(dst);
        std::size_t offset = 0;
        while (running.load() && offset < size) {
            const ssize_t received = ::recv(fd, bytes + offset, size - offset, 0);
            if (received > 0) {
                offset += static_cast<std::size_t>(received);
                continue;
            }
            if (received == 0) {
                return false;
            }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return false;
        }
        return offset == size;
    }

    bool writeAll(int fd, const void* src, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(src);
        std::size_t offset = 0;
        while (running.load() && offset < size) {
            const ssize_t sent = ::send(fd, bytes + offset, size - offset, 0);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return offset == size;
    }

    bool sendAck(int fd,
                 char command,
                 libera::etherdream::PlaybackState playbackState,
                 std::uint16_t bufferFullness,
                 std::uint32_t pointRate) {
        std::array<std::uint8_t, 22> data{};
        data[0] = static_cast<std::uint8_t>('a');
        data[1] = static_cast<std::uint8_t>(command);
        data[2] = 0;
        data[3] = static_cast<std::uint8_t>(libera::etherdream::LightEngineState::Ready);
        data[4] = static_cast<std::uint8_t>(playbackState);
        data[5] = 0;
        putLe16(data, 6, 0);
        putLe16(data, 8, 0);
        putLe16(data, 10, 0);
        putLe16(data, 12, bufferFullness);
        putLe32(data, 14, pointRate);
        putLe32(data, 18, 0);
        return writeAll(fd, data.data(), data.size());
    }

    void fail(std::string message) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (violationMessage.empty()) {
                violationMessage = std::move(message);
            }
        }
        condition.notify_all();
    }

    void markRecoveredBegin() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            recoveredBeginSeen = true;
        }
        condition.notify_all();
    }

    void handleClient(int client, int connectionIndex) {
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        if (!sendAck(client, '?', libera::etherdream::PlaybackState::Idle, 0, 0)) {
            return;
        }

        bool preparedSeen = false;
        bool dataSeen = false;
        std::uint16_t bufferedPoints = 0;
        int beginCommandsOnThisConnection = 0;

        while (running.load()) {
            std::uint8_t opcode = 0;
            if (!readExact(client, &opcode, 1)) {
                return;
            }

            const char command = static_cast<char>(opcode);

            if (command == 'p') {
                preparedSeen = true;
                dataSeen = false;
                bufferedPoints = 0;
                sendAck(client, 'p', libera::etherdream::PlaybackState::Prepared, 0, 0);
                continue;
            }

            if (command == 'd') {
                std::array<std::uint8_t, 2> countBytes{};
                if (!readExact(client, countBytes.data(), countBytes.size())) {
                    return;
                }
                const std::uint16_t pointCount = readLe16(countBytes.data());
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(pointCount) * 18u);
                if (!readExact(client, payload.data(), payload.size())) {
                    return;
                }
                if (!preparedSeen) {
                    fail("data command arrived before prepare");
                    return;
                }
                dataSeen = true;
                bufferedPoints = static_cast<std::uint16_t>(bufferedPoints + pointCount);
                sendAck(client, 'd', libera::etherdream::PlaybackState::Prepared, bufferedPoints, 0);
                continue;
            }

            if (command == 'b') {
                std::array<std::uint8_t, 6> beginBytes{};
                if (!readExact(client, beginBytes.data(), beginBytes.size())) {
                    return;
                }
                if (!dataSeen) {
                    fail("begin command arrived before data");
                    return;
                }

                ++beginCommandsOnThisConnection;
                if (connectionIndex == 1) {
                    if (beginCommandsOnThisConnection > 1) {
                        fail("begin was retried on the stale timed-out TCP connection");
                    }
                    // Simulate a lost begin ACK while keeping the TCP socket
                    // open. The controller must close and reconnect rather
                    // than send another lifecycle command on this stream.
                    continue;
                }

                sendAck(client, 'b', libera::etherdream::PlaybackState::Playing, bufferedPoints, 30000);
                markRecoveredBegin();
                return;
            }

            if (command == 's') {
                sendAck(client, 's', libera::etherdream::PlaybackState::Idle, 0, 0);
                continue;
            }

            fail(std::string("unexpected command: ") + command);
            return;
        }
    }

    void run() {
        while (running.load()) {
            const int client = ::accept(listenFd, nullptr, nullptr);
            if (client < 0) {
                if (!running.load()) {
                    break;
                }
                continue;
            }

            clientFd.store(client);
            const int connectionIndex = acceptedConnections.fetch_add(1) + 1;
            handleClient(client, connectionIndex);

            const int activeClient = clientFd.exchange(-1);
            if (activeClient >= 0) {
                ::shutdown(activeClient, SHUT_RDWR);
                ::close(activeClient);
            }
        }
    }

    int listenFd = -1;
    unsigned short portNumber = 0;
    std::atomic<bool> running{false};
    std::atomic<int> clientFd{-1};
    std::atomic<int> acceptedConnections{0};
    std::thread serverThread;
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool recoveredBeginSeen = false;
    std::string violationMessage;
};

libera::core::Frame makeFrame(std::size_t pointCount) {
    libera::core::Frame frame;
    frame.time = std::chrono::steady_clock::now();
    frame.points.reserve(pointCount);
    for (std::size_t i = 0; i < pointCount; ++i) {
        libera::core::LaserPoint point;
        point.x = static_cast<float>((i % 100) / 100.0);
        point.y = 0.0f;
        point.r = 1.0f;
        frame.points.push_back(point);
    }
    return frame;
}

} // namespace

int main() {
    BeginTimeoutServer server;
    libera::core::LaserController::setTargetLatency(0ms);

    libera::etherdream::EtherDreamController controller;
    controller.setPointRate(30000);
    controller.setArmed(true);
    controller.startFrameMode();

    ASSERT_TRUE(controller.sendFrame(makeFrame(1000)), "frame queued");

    libera::etherdream::EtherDreamControllerInfo info{
        "loopback",
        "Loopback",
        "127.0.0.1",
        server.port(),
        4096};

    auto connected = controller.connect(info);
    ASSERT_TRUE(connected, "connect should succeed");

    controller.startThread();

    const bool recovered = server.waitForRecoveredBegin(5s);

    controller.stopThread();
    controller.close();
    server.stop();

    if (!recovered) {
        const auto violation = server.violation();
        if (!violation.empty()) {
            std::fprintf(stderr, "EtherDream begin timeout reconnect violation: %s\n",
                         violation.c_str());
        } else {
            std::fprintf(stderr,
                         "EtherDream begin timeout reconnect timed out after %d connection(s)\n",
                         server.connectionCount());
        }
        return 1;
    }

    ASSERT_TRUE(server.connectionCount() >= 2, "controller should reconnect after begin timeout");

    std::puts("EtherDream begin timeout reconnect test passed.");
    return 0;
}

#endif // _WIN32
