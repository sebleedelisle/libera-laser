#include "libera/etherdream/EtherDreamDiscoverer.hpp"

#include "libera/etherdream/EtherDreamDeviceInfo.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/log/Log.hpp"
#if defined(_WIN32)
#  include <winsock2.h>
#endif

#include <array>
#include <cstdio>

namespace libera::etherdream {
namespace {

constexpr std::size_t kMinDiscoveryPacketBytes = 36;

std::uint16_t read_le_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t read_le_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

std::string format_mac_id(std::uint64_t mac) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04X%04X%04X",
                  static_cast<unsigned>((mac >> 32) & 0xFFFFu),
                  static_cast<unsigned>((mac >> 16) & 0xFFFFu),
                  static_cast<unsigned>(mac & 0xFFFFu));
    return std::string(buffer.data());
}

} // namespace

EtherDreamDiscoverer::EtherDreamDiscoverer() {
    io = net::shared_io_context();
    socket = std::make_unique<net::UdpSocket>(*io);

    std::error_code ec;
    if ((ec = socket->open_v4())) {
        logError("[EtherDreamDiscoverer] Failed to open UDP socket", ec.message());
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
        logError("[EtherDreamDiscoverer] Failed to bind UDP socket", ec.message());
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

std::vector<std::unique_ptr<core::DacInfo>>
EtherDreamDiscoverer::discover() {
    std::vector<std::unique_ptr<core::DacInfo>> results;
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

        if (received < kMinDiscoveryPacketBytes) {
            continue;
        }

        const std::uint8_t* data = buffer.data();
        std::uint64_t mac = 0;
        for (int i = 0; i < 6; ++i) {
            mac = (mac << 8) | static_cast<std::uint64_t>(data[i]);
        }
        const std::uint8_t* cursor = data + 6;
        const auto hardwareRevision = read_le_u16(cursor); cursor += 2;
        const auto softwareRevision = read_le_u16(cursor); cursor += 2;
        const auto bufferCapacity = read_le_u16(cursor); cursor += 2;
        const auto maxPointRate = read_le_u32(cursor); cursor += 4;
        (void)cursor; // remaining status bytes are currently unused.

        std::string id = mac ? format_mac_id(mac) : ("etherdream-" + ip);
        std::string label = "EtherDream @ " + ip;
        std::string hardwareVersion =
            "hw" + std::to_string(hardwareRevision) + "-sw" + std::to_string(softwareRevision);

        EtherDreamDeviceInfo info{
            id,
            label,
            ip,
            dacPort,
            static_cast<int>(bufferCapacity),
            std::move(hardwareVersion),
            maxPointRate};

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
