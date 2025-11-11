#include "libera/etherdream/EtherDreamDevice.hpp"

#ifdef _WIN32
#include <cstdio>

int main() {
    std::puts("Skipping EtherDream reconnect test on Windows.");
    return 0;
}

#else

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

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

class DummyTcpServer {
public:
    DummyTcpServer() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_TRUE(listenFd_ >= 0, "socket");

        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // let the OS choose

        ASSERT_TRUE(::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        ASSERT_TRUE(::listen(listenFd_, 4) == 0, "listen");

        socklen_t len = sizeof(addr);
        ASSERT_TRUE(::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "getsockname");
        port_ = ntohs(addr.sin_port);

        running_.store(true);
        thread_ = std::thread([this]{ this->run(); });
    }

    ~DummyTcpServer() {
        stop();
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return; // already stopped
        }
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        if (thread_.joinable()) {
            thread_.join();
        }
        for (int fd : accepted_) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        accepted_.clear();
    }

    unsigned short port() const { return port_; }

    int connectionsAccepted() const { return acceptedCount_.load(); }

private:
    void run() {
        while (running_.load()) {
            int client = ::accept(listenFd_, nullptr, nullptr);
            if (client < 0) {
                if (!running_.load()) {
                    break;
                }
                continue;
            }
            acceptedCount_.fetch_add(1);
            accepted_.push_back(client);
        }
    }

    int listenFd_ = -1;
    unsigned short port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> acceptedCount_{0};
    std::thread thread_;
    std::vector<int> accepted_;
};

} // namespace

int main() {
    DummyTcpServer server;

    libera::etherdream::EtherDreamDevice device;
    libera::etherdream::EtherDreamDeviceInfo info{"loopback", "Loopback", "127.0.0.1", server.port()};

    constexpr int kTries = 3000;
    for (int i = 0; i < kTries; ++i) {
        auto connected = device.connect(info);
        ASSERT_TRUE(connected, "connect should succeed");

        device.close();

        // Give the server a moment to observe the close
        std::this_thread::sleep_for(50ms);
    }

    ASSERT_TRUE(server.connectionsAccepted() >= kTries, "server observed all connections");

    server.stop();
    std::puts("EtherDream reconnect test passed.");
    return 0;
}

#endif // _WIN32
