#include "libera/etherdream/EtherDreamDiscoverer.hpp"

#include "libera/etherdream/EtherDreamDeviceInfo.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/log/Log.hpp"
#if defined(_WIN32)
#  include <winsock2.h>
#endif

#include <array>

namespace libera::etherdream {

EtherDreamDiscoverer::EtherDreamDiscoverer() {
    io = net::shared_io_context();
    socket = std::make_unique<net::UdpSocket>(*io);

    std::error_code ec;
    if ((ec = socket->open_v4())) {
        logError("[EtherDreamDiscoverer] Failed to open UDP socket: ", ec.message(), "\n");
        socket.reset();
        return;
    }

#if defined(_WIN32)
    if (!ec) {
        BOOL exclusive = FALSE;
        ::setsockopt(socket->raw().native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    }
#endif

    socket->raw().set_option(asio::socket_base::reuse_address(true), ec);
    if ((ec = socket->bind_any(config::ETHERDREAM_DISCOVERY_PORT))) {
        logError("[EtherDreamDiscoverer] Failed to bind UDP socket: ", ec.message(), "\n");
        socket->close();
        socket.reset();
        return;
    }

    running.store(true);
    listener = std::thread([this]{ threadedFunction(); });
}

EtherDreamDiscoverer::~EtherDreamDiscoverer() {
    running.store(false);
    if (socket) {
        socket->close();
    }
    if (listener.joinable()) {
        listener.join();
    }
}

std::vector<std::unique_ptr<core::DiscoveredDac>>
EtherDreamDiscoverer::discover() {
    std::vector<std::unique_ptr<core::DiscoveredDac>> results;
    const auto now = Clock::now();
    std::lock_guard lock(devicesMutex);
    pruneStaleUnlocked(now);
    results.reserve(devices.size());
    for (const auto& [id, entry] : devices) {
        results.emplace_back(std::make_unique<EtherDreamDeviceInfo>(entry.info));
    }
    return results;
}

void EtherDreamDiscoverer::threadedFunction() {
    if (!socket) {
        return;
    }

    std::array<std::uint8_t, 128> buffer{};
    while (running.load()) {
        asio::ip::udp::endpoint sender;
        std::size_t received = 0;
        auto ec = socket->recv_from(buffer.data(), buffer.size(), sender, received,
                                    std::chrono::milliseconds(1000));
        auto now = Clock::now();

        if (!running.load()) {
            break;
        }

        if (ec == asio::error::operation_aborted) {
            break;
        }

        if (ec == asio::error::timed_out) {
            std::lock_guard lock(devicesMutex);
            pruneStaleUnlocked(now);
            continue;
        }

        if (ec) {
            continue;
        }

        std::string ip = sender.address().to_string();
        unsigned short dacPort = config::ETHERDREAM_DAC_PORT_DEFAULT; // Broadcast packets omit TCP port; use default.

        std::string id = "etherdream-" + ip;
        std::string label = "EtherDream @ " + ip;

        EtherDreamDeviceInfo info{id, label, ip, dacPort};

        {
            std::lock_guard lock(devicesMutex);
            devices.insert_or_assign(id, DeviceEntry{info, now});
            pruneStaleUnlocked(now);
        }
    }
}

void EtherDreamDiscoverer::pruneStaleUnlocked(Clock::time_point now) {
    const auto expiry = now - config::ETHERDREAM_DISCOVERY_TIMEOUT;
    for (auto it = devices.begin(); it != devices.end(); ) {
        if (it->second.lastSeen < expiry) {
            it = devices.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace libera::etherdream
