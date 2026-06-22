#include "libera/etherdream/EtherDreamController.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/core/LaserPoint.hpp"
#include "libera/log/Log.hpp"

#ifdef _WIN32
#include <cstdio>

int main() {
    std::puts("Skipping EtherDream stream order test on Windows.");
    return 0;
}

#else

#include <array>
#include <atomic>
#include <cerrno>
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

std::uint32_t readLe32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

class EtherDreamLoopbackServer {
public:
    explicit EtherDreamLoopbackServer(
        libera::etherdream::PlaybackState initialPlaybackState =
            libera::etherdream::PlaybackState::Idle,
        std::uint16_t preparedBufferAfterPrepare = 0,
        bool injectUnderflowAfterFirstBegin = false,
        bool injectPlaybackIdleNakAfterFirstBegin = false,
        bool injectBufferFullNakAfterFirstBegin = false,
        bool injectStopConditionOnFirstClear = false)
    : initialPlaybackState(initialPlaybackState)
    , preparedBufferAfterPrepare(preparedBufferAfterPrepare)
    , injectUnderflowAfterFirstBegin(injectUnderflowAfterFirstBegin)
    , injectPlaybackIdleNakAfterFirstBegin(injectPlaybackIdleNakAfterFirstBegin)
    , injectBufferFullNakAfterFirstBegin(injectBufferFullNakAfterFirstBegin)
    , injectStopConditionOnFirstClear(injectStopConditionOnFirstClear) {
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

    ~EtherDreamLoopbackServer() {
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

    bool waitForBegin(std::chrono::milliseconds timeout) {
        return waitForBeginCount(1, timeout);
    }

    bool waitForBeginCount(std::size_t expectedBeginCount,
                           std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, timeout, [this, expectedBeginCount] {
            return beginCount >= expectedBeginCount || !violationMessage.empty();
        }) && beginCount >= expectedBeginCount && violationMessage.empty();
    }

    std::string violation() const {
        std::lock_guard<std::mutex> lock(mutex);
        return violationMessage;
    }

    std::vector<char> commands() const {
        std::lock_guard<std::mutex> lock(mutex);
        return commandLog;
    }

    std::vector<std::uint32_t> beginRates() const {
        std::lock_guard<std::mutex> lock(mutex);
        return beginRateLog;
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
                 char response,
                 char command,
                 libera::etherdream::PlaybackState playbackState,
                 std::uint16_t bufferFullness,
                 std::uint32_t pointRate,
                 std::uint16_t playbackFlags = 0,
                 libera::etherdream::LightEngineState lightEngineState =
                     libera::etherdream::LightEngineState::Ready,
                 std::uint16_t lightEngineFlags = 0) {
        std::array<std::uint8_t, 22> data{};
        data[0] = static_cast<std::uint8_t>(response);
        data[1] = static_cast<std::uint8_t>(command);
        data[2] = 0; // protocol
        data[3] = static_cast<std::uint8_t>(lightEngineState);
        data[4] = static_cast<std::uint8_t>(playbackState);
        data[5] = 0; // source
        putLe16(data, 6, lightEngineFlags);
        putLe16(data, 8, playbackFlags);
        putLe16(data, 10, 0); // source flags
        putLe16(data, 12, bufferFullness);
        putLe32(data, 14, pointRate);
        putLe32(data, 18, 0); // point count
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

    void recordCommand(char command) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            commandLog.push_back(command);
        }
        condition.notify_all();
    }

    void run() {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            return;
        }
        clientFd.store(client);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        const std::uint16_t initialBuffer =
            initialPlaybackState == libera::etherdream::PlaybackState::Prepared ? 1000 : 0;
        const bool stopConditionInitiallyActive = injectStopConditionOnFirstClear;
        if (!sendAck(client,
                     'a',
                     '?',
                     initialPlaybackState,
                     initialBuffer,
                     0,
                     stopConditionInitiallyActive
                         ? libera::etherdream::EtherDreamStatus::PlaybackFlagEstop
                         : 0,
                     stopConditionInitiallyActive
                         ? libera::etherdream::LightEngineState::Estop
                         : libera::etherdream::LightEngineState::Ready,
                     stopConditionInitiallyActive ? 0x1u : 0u)) {
            return;
        }

        const bool startupResetRequired =
            initialPlaybackState != libera::etherdream::PlaybackState::Idle
            && !stopConditionInitiallyActive;
        bool stopSeen = false;
        bool preparedSeen = false;
        bool firstDataSeen = false;
        bool underflowInjected = false;
        bool playbackIdleNakInjected = false;
        bool bufferFullNakInjected = false;
        bool stopConditionInjected = false;
        std::size_t localBeginCount = 0;
        std::uint16_t bufferedPoints = 0;

        while (running.load()) {
            std::uint8_t opcode = 0;
            if (!readExact(client, &opcode, 1)) {
                continue;
            }

            const char command = static_cast<char>(opcode);
            recordCommand(command);

            if (startupResetRequired && !stopSeen && command != 's') {
                fail("non-idle startup was not stopped before streaming commands");
                return;
            }

            if (command == 's') {
                stopSeen = true;
                preparedSeen = false;
                firstDataSeen = false;
                bufferedPoints = 0;
                sendAck(client, 'a', 's', libera::etherdream::PlaybackState::Idle, 0, 0);
                continue;
            }

            if (command == 'c') {
                if (injectStopConditionOnFirstClear && !stopConditionInjected) {
                    stopConditionInjected = true;
                    sendAck(client,
                            '!',
                            'c',
                            libera::etherdream::PlaybackState::Idle,
                            0,
                            0,
                            libera::etherdream::EtherDreamStatus::PlaybackFlagEstop,
                            libera::etherdream::LightEngineState::Estop,
                            0x1u);
                    continue;
                }

                sendAck(client,
                        'a',
                        'c',
                        libera::etherdream::PlaybackState::Idle,
                        0,
                        0);
                continue;
            }

            if (command == 'p') {
                preparedSeen = true;
                bufferedPoints = preparedBufferAfterPrepare;
                sendAck(client, 'a', 'p', libera::etherdream::PlaybackState::Prepared, bufferedPoints, 0);
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
                if (injectUnderflowAfterFirstBegin
                    && localBeginCount == 1
                    && !underflowInjected) {
                    underflowInjected = true;
                    preparedSeen = false;
                    firstDataSeen = false;
                    bufferedPoints = 0;
                    sendAck(client,
                            'a',
                            'd',
                            libera::etherdream::PlaybackState::Idle,
                            0,
                            0,
                            libera::etherdream::EtherDreamStatus::PlaybackFlagUnderflow);
                    continue;
                }
                if (injectPlaybackIdleNakAfterFirstBegin
                    && localBeginCount == 1
                    && !playbackIdleNakInjected) {
                    playbackIdleNakInjected = true;
                    preparedSeen = false;
                    firstDataSeen = false;
                    bufferedPoints = 0;
                    sendAck(client,
                            'I',
                            'd',
                            libera::etherdream::PlaybackState::Idle,
                            0,
                            0);
                    continue;
                }
                if (injectBufferFullNakAfterFirstBegin
                    && localBeginCount == 1
                    && !bufferFullNakInjected) {
                    bufferFullNakInjected = true;
                    preparedSeen = true;
                    firstDataSeen = true;
                    bufferedPoints = 4096;
                    sendAck(client,
                            'I',
                            'd',
                            libera::etherdream::PlaybackState::Prepared,
                            bufferedPoints,
                            0);
                    continue;
                }
                if (firstDataSeen) {
                    fail("second data command arrived before begin");
                    return;
                }
                if (pointCount > libera::etherdream::config::ETHERDREAM_MAX_PACKET_POINTS) {
                    fail("data command exceeded Ether Dream packet limit");
                    return;
                }
                if (static_cast<std::size_t>(bufferedPoints) + pointCount > 4096u) {
                    fail("data command exceeded simulated Ether Dream FIFO capacity");
                    return;
                }

                firstDataSeen = true;
                bufferedPoints = static_cast<std::uint16_t>(bufferedPoints + pointCount);
                sendAck(client, 'a', 'd', libera::etherdream::PlaybackState::Prepared, bufferedPoints, 0);
                continue;
            }

            if (command == 'b') {
                std::array<std::uint8_t, 6> beginBytes{};
                if (!readExact(client, beginBytes.data(), beginBytes.size())) {
                    return;
                }
                const auto beginRate = readLe32(beginBytes.data() + 2);
                if (!firstDataSeen) {
                    fail("begin command arrived before any data was buffered");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++localBeginCount;
                    beginCount = localBeginCount;
                    beginRateLog.push_back(beginRate);
                }
                sendAck(client,
                        'a',
                        'b',
                        libera::etherdream::PlaybackState::Playing,
                        bufferedPoints,
                        beginRate);
                condition.notify_all();
                continue;
            }

            if (command == 'q') {
                std::array<std::uint8_t, 4> rateBytes{};
                if (!readExact(client, rateBytes.data(), rateBytes.size())) {
                    return;
                }
                fail("point-rate command arrived before begin");
                return;
            }

            if (command == '?') {
                sendAck(client, 'a', '?', libera::etherdream::PlaybackState::Prepared, bufferedPoints, 0);
                continue;
            }

            fail(std::string("unexpected command: ") + command);
            return;
        }
    }

    int listenFd = -1;
    unsigned short portNumber = 0;
    std::atomic<bool> running{false};
    std::atomic<int> clientFd{-1};
    std::thread serverThread;
    libera::etherdream::PlaybackState initialPlaybackState;
    std::uint16_t preparedBufferAfterPrepare = 0;
    bool injectUnderflowAfterFirstBegin = false;
    bool injectPlaybackIdleNakAfterFirstBegin = false;
    bool injectBufferFullNakAfterFirstBegin = false;
    bool injectStopConditionOnFirstClear = false;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<char> commandLog;
    std::vector<std::uint32_t> beginRateLog;
    std::size_t beginCount = 0;
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

bool runStreamOrderScenario(libera::etherdream::PlaybackState initialPlaybackState,
                            const char* scenarioName,
                            std::uint16_t preparedBufferAfterPrepare = 0,
                            bool injectUnderflowAfterFirstBegin = false,
                            std::size_t expectedBeginCount = 1,
                            bool expectUnderflowRecorded = false,
                            bool injectPlaybackIdleNakAfterFirstBegin = false,
                            bool expectPlaybackIdleRecorded = false,
                            bool injectBufferFullNakAfterFirstBegin = false,
                            bool expectBufferOverrunRecorded = false,
                            bool injectStopConditionOnFirstClear = false,
                            bool expectStopConditionRecorded = false,
                            bool changePointRateAfterFirstBegin = false) {
    EtherDreamLoopbackServer server(initialPlaybackState,
                                    preparedBufferAfterPrepare,
                                    injectUnderflowAfterFirstBegin,
                                    injectPlaybackIdleNakAfterFirstBegin,
                                    injectBufferFullNakAfterFirstBegin,
                                    injectStopConditionOnFirstClear);
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

    bool began = false;
    if (changePointRateAfterFirstBegin) {
        began = server.waitForBeginCount(1, 2s);
        if (began) {
            controller.setPointRate(20000);
            ASSERT_TRUE(controller.sendFrame(makeFrame(1000)),
                        "second frame queued after point-rate change");
            began = server.waitForBeginCount(expectedBeginCount, 2s);
        }
    } else {
        began = server.waitForBeginCount(expectedBeginCount, 2s);
    }

    controller.stopThread();
    controller.close();
    server.stop();

    if (!began) {
        const auto violation = server.violation();
        if (!violation.empty()) {
            std::fprintf(stderr, "EtherDream stream order violation (%s): %s\n",
                         scenarioName,
                         violation.c_str());
        } else {
            const auto commands = server.commands();
            std::fprintf(stderr, "EtherDream stream order timed out (%s). Commands:",
                         scenarioName);
            for (char command : commands) {
                std::fprintf(stderr, " %c", command);
            }
            std::fprintf(stderr, "\n");
        }
        return false;
    }

    if (expectUnderflowRecorded) {
        bool underflowRecorded = false;
        for (const auto& error : controller.getErrors()) {
            if (error.code == "network.buffer_underflow" && error.count > 0) {
                underflowRecorded = true;
                break;
            }
        }
        if (!underflowRecorded) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): underflow was not recorded\n",
                         scenarioName);
            return false;
        }
    }

    if (expectPlaybackIdleRecorded) {
        bool playbackIdleRecorded = false;
        bool protocolErrorRecorded = false;
        for (const auto& error : controller.getErrors()) {
            if (error.code == "etherdream.playback_idle" && error.count > 0) {
                playbackIdleRecorded = true;
            }
            if (error.code == "network.protocol_error" && error.count > 0) {
                protocolErrorRecorded = true;
            }
        }
        if (!playbackIdleRecorded) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): playback idle was not recorded\n",
                         scenarioName);
            return false;
        }
        if (protocolErrorRecorded) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): playback idle was reported as protocol error\n",
                         scenarioName);
            return false;
        }
    }

    if (expectBufferOverrunRecorded) {
        bool bufferOverrunRecorded = false;
        bool protocolErrorRecorded = false;
        for (const auto& error : controller.getErrors()) {
            if (error.code == "network.buffer_overrun" && error.count > 0) {
                bufferOverrunRecorded = true;
            }
            if (error.code == "network.protocol_error" && error.count > 0) {
                protocolErrorRecorded = true;
            }
        }
        if (!bufferOverrunRecorded) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): buffer overrun was not recorded\n",
                         scenarioName);
            return false;
        }
        if (protocolErrorRecorded) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): full data rejection was reported as protocol error\n",
                         scenarioName);
            return false;
        }
    }

    if (expectStopConditionRecorded) {
        bool stopConditionRecorded = false;
        bool protocolErrorRecorded = false;
        for (const auto& error : controller.getErrors()) {
            if (error.code == "etherdream.stop_condition" && error.count > 0) {
                stopConditionRecorded = true;
            }
            if (error.code == "network.protocol_error" && error.count > 0) {
                protocolErrorRecorded = true;
            }
        }
        if (!stopConditionRecorded) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): stop condition was not recorded\n",
                         scenarioName);
            return false;
        }
        if (protocolErrorRecorded) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): stop condition was reported as protocol error\n",
                         scenarioName);
            return false;
        }
    }

    if (changePointRateAfterFirstBegin) {
        const auto commands = server.commands();
        for (char command : commands) {
            if (command == 'q') {
                std::fprintf(stderr,
                             "EtherDream stream order violation (%s): point-rate change used q instead of restart\n",
                             scenarioName);
                return false;
            }
        }
        const auto beginRates = server.beginRates();
        if (beginRates.size() < 2 || beginRates[0] != 30000u || beginRates[1] != 20000u) {
            std::fprintf(stderr,
                         "EtherDream stream order violation (%s): begin rates did not show restart at new rate\n",
                         scenarioName);
            return false;
        }
    }

    return true;
}

int main() {
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Idle, "idle startup")) {
        return 1;
    }
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Prepared, "prepared startup")) {
        return 1;
    }
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Idle,
                                "partial prepared buffer after prepare",
                                static_cast<std::uint16_t>(
                                    libera::etherdream::config::ETHERDREAM_MIN_PACKET_POINTS - 1))) {
        return 1;
    }
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Idle,
                                "underflow recovery",
                                0,
                                true,
                                2,
                                true)) {
        return 1;
    }
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Idle,
                                "playback idle NAK recovery",
                                0,
                                false,
                                2,
                                false,
                                true,
                                true)) {
        return 1;
    }
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Idle,
                                "full data NAK classification",
                                0,
                                false,
                                2,
                                false,
                                false,
                                false,
                                true,
                                true)) {
        return 1;
    }
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Idle,
                                "stop condition recovery",
                                0,
                                false,
                                1,
                                false,
                                false,
                                false,
                                false,
                                false,
                                true,
                                true)) {
        return 1;
    }
    if (!runStreamOrderScenario(libera::etherdream::PlaybackState::Idle,
                                "point-rate change restart",
                                0,
                                false,
                                2,
                                false,
                                false,
                                false,
                                false,
                                false,
                                false,
                                false,
                                true)) {
        return 1;
    }

    std::puts("EtherDream stream order test passed.");
    return 0;
}

#endif // _WIN32
