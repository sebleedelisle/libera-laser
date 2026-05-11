#include "libera/etherdream/EtherDreamManager.hpp"

#include "libera/core/ByteRead.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/log/Log.hpp"
#if defined(_WIN32)
#  include <winsock2.h>
#endif

#include <array>
#include <cstdio>
#include <limits>

namespace libera::etherdream {
namespace {

constexpr std::size_t MIN_DISCOVERY_PACKET_BYTES = 36;

unsigned short discovery_port_from_revisions(std::uint16_t hardwareRevision,
                                             std::uint16_t softwareRevision) {
    // Historical compatibility with the old EtherDream emulator:
    // hardwareRevision == 0 means "virtual EtherDream", and the software
    // revision field is repurposed to carry the TCP port offset from 7765.
    if (hardwareRevision != 0) {
        return config::ETHERDREAM_DAC_PORT_DEFAULT;
    }

    const auto port = static_cast<unsigned int>(config::ETHERDREAM_DAC_PORT_DEFAULT) +
                      static_cast<unsigned int>(softwareRevision);
    if (port > static_cast<unsigned int>(std::numeric_limits<unsigned short>::max())) {
        return config::ETHERDREAM_DAC_PORT_DEFAULT;
    }
    return static_cast<unsigned short>(port);
}

std::string format_mac_id(std::uint64_t mac) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04X%04X%04X",
                  static_cast<unsigned>((mac >> 32) & 0xFFFFu),
                  static_cast<unsigned>((mac >> 16) & 0xFFFFu),
                  static_cast<unsigned>(mac & 0xFFFFu));
    return std::string(buffer.data());
}

std::string format_display_id(std::uint64_t mac) {
    std::array<char, 16> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%06x",
                  static_cast<unsigned>(mac & 0xFFFFFFu));
    return std::string(buffer.data());
}

} // namespace

EtherDreamManager::EtherDreamManager() {
    io = net::shared_io_context();
    socket = std::make_unique<net::UdpSocket>(*io);

    std::error_code ec;
    if ((ec = socket->open_v4())) {
        logError("[EtherDreamManager] Failed to open UDP socket", ec.message());
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
        logError("[EtherDreamManager] Failed to bind UDP socket", ec.message());
        socket->close();
        socket.reset();
        return;
    }

    running.store(true);
    listenerFinished.store(false, std::memory_order_relaxed);
    listener = std::thread([this]{
        threadedFunction();
        listenerFinished.store(true, std::memory_order_release);
    });
}

EtherDreamManager::~EtherDreamManager() {
    closeAll();
}

std::vector<std::unique_ptr<core::ControllerInfo>>
EtherDreamManager::discover() {
    std::vector<std::unique_ptr<core::ControllerInfo>> results;
    const auto now = Clock::now();
    std::lock_guard lock(controllersMutex);
    pruneStaleUnlocked(now);
    results.reserve(controllers.size());
    for (const auto& [id, entry] : controllers) {
        results.emplace_back(std::make_unique<EtherDreamControllerInfo>(entry.info));
    }
    return results;
}

std::shared_ptr<EtherDreamController>
EtherDreamManager::createController(const EtherDreamControllerInfo& info) {
    return std::make_shared<EtherDreamController>(info);
}

EtherDreamManager::NewControllerDisposition
EtherDreamManager::prepareNewController(EtherDreamController& controller,
                                        const EtherDreamControllerInfo& info) {
    // Connect/start only once for a new instance. Existing instances keep
    // their own reconnect loop in the controller thread.
    if (auto result = controller.connect(info); !result) {
        logError("[EtherDreamManager] initial connect failed", result.error().message());
    }
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void EtherDreamManager::beforeCloseControllers() {
    running.store(false);
    if (socket) {
        socket->close();
        socket.reset();
    }
    core::timedJoin(listener, listenerFinished, std::chrono::milliseconds(3000),
                    "EtherDreamManager::listener");
}

void EtherDreamManager::afterCloseControllers() {
    std::lock_guard lock(controllersMutex);
    controllers.clear();
}

void EtherDreamManager::closeController(const std::string& key,
                                        EtherDreamController& controller) {
    (void)key;
    controller.close();
}

void EtherDreamManager::threadedFunction() {
    if (!socket) {
        return;
    }

    std::array<std::uint8_t, 128> buffer{};
    while (running.load()) {
        asio::ip::udp::endpoint sender;
        std::size_t received = 0;
        auto ec = socket->recv_from(buffer.data(), buffer.size(), sender, received,
                                    std::chrono::milliseconds(1000), false);
        auto now = Clock::now();

        if (!running.load()) {
            break;
        }

        if (ec == asio::error::operation_aborted) {
            break;
        }

        if (ec == asio::error::timed_out) {
            std::lock_guard lock(controllersMutex);
            pruneStaleUnlocked(now);
            continue;
        }

        if (ec) {
            continue;
        }

        std::string ip = sender.address().to_string();

        if (received < MIN_DISCOVERY_PACKET_BYTES) {
            continue;
        }

        const std::uint8_t* data = buffer.data();
        std::uint64_t mac = 0;
        for (int i = 0; i < 6; ++i) {
            mac = (mac << 8) | static_cast<std::uint64_t>(data[i]);
        }
        const std::uint8_t* cursor = data + 6;
        const auto hardwareRevision = core::bytes::readLe16(cursor); cursor += 2;
        const auto softwareRevision = core::bytes::readLe16(cursor); cursor += 2;
        const auto bufferCapacity = core::bytes::readLe16(cursor); cursor += 2;
        const auto maxPointRate = core::bytes::readLe32(cursor); cursor += 4;
        const unsigned short port = discovery_port_from_revisions(hardwareRevision, softwareRevision);
        core::ControllerUsageState usageState = core::ControllerUsageState::Unknown;
        switch (static_cast<PlaybackState>(cursor[2])) {
            case PlaybackState::Idle:
                usageState = core::ControllerUsageState::Idle;
                break;
            case PlaybackState::Prepared:
            case PlaybackState::Playing:
            case PlaybackState::Paused:
                usageState = core::ControllerUsageState::Active;
                break;
            default:
                usageState = core::ControllerUsageState::Unknown;
                break;
        }

        std::string id = mac ? format_mac_id(mac) : ("etherdream-" + ip);
        // Keep the full stable id for reconnect/persistence, but use the short
        // hardware id for the friendly label so the UI shows "Ether Dream
        // 123abc" instead of the current IP address.
        std::string label = mac
            ? ("Ether Dream " + format_display_id(mac))
            : ("Ether Dream " + ip);
        std::string hardwareVersion =
            "hw" + std::to_string(hardwareRevision) + "-sw" + std::to_string(softwareRevision);

        EtherDreamControllerInfo info{
            id,
            label,
            ip,
            port,
            static_cast<int>(bufferCapacity),
            std::move(hardwareVersion),
            maxPointRate};
        info.setUsageState(usageState);

        {
            std::lock_guard lock(controllersMutex);
            controllers.insert_or_assign(id, ControllerEntry{info, now});
            pruneStaleUnlocked(now);
        }
    }
}

void EtherDreamManager::pruneStaleUnlocked(Clock::time_point now) {
    const auto expiry = now - config::ETHERDREAM_DISCOVERY_TIMEOUT;
    for (auto it = controllers.begin(); it != controllers.end(); ) {
        if (it->second.lastSeen < expiry) {
            it = controllers.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace libera::etherdream
