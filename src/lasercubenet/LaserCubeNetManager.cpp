#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubenet/LaserCubeNetController.hpp"

#include "libera/log/Log.hpp"

#include <array>
#include <iomanip>
#include <sstream>

namespace libera::lasercubenet {
namespace {
[[maybe_unused]] std::string hexPrefix(const std::uint8_t* data, std::size_t size, std::size_t maxBytes = 16) {
    if (!data || size == 0) {
        return {};
    }
    const std::size_t count = std::min(size, maxBytes);
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    if (size > count) {
        oss << " ...";
    }
    return oss.str();
}
}

LaserCubeNetManager::LaserCubeNetManager() {
    io = net::shared_io_context();
    socket = std::make_unique<net::UdpSocket>(*io);
    if (!socket->open_v4()) {
        socket->enable_broadcast(true);
        // Bind to the command port so we can receive status replies.
        socket->bind_any(LaserCubeNetConfig::COMMAND_PORT);
    }
    running.store(true);
    // Dedicated discovery thread so controller scanning never blocks the caller.
    listener = std::thread([this]{ discoveryThread(); });
}

LaserCubeNetManager::~LaserCubeNetManager() {
    closeAll();
}

void LaserCubeNetManager::closeAll() {
    running.store(false);
    if (socket) {
        socket->close();
    }
    if (listener.joinable()) {
        listener.join();
    }

    std::unordered_map<std::string, std::shared_ptr<LaserCubeNetController>> snapshot;
    {
        std::lock_guard lock(activeMutex);
        for (auto& [id, weak] : active) {
            if (auto dev = weak.lock()) {
                snapshot.emplace(id, std::move(dev));
            }
        }
        active.clear();
    }

    for (auto& [id, dev] : snapshot) {
        if (dev) {
            dev->stop();
            dev->close();
        }
    }

    std::lock_guard lock(controllersMutex);
    controllers.clear();
}

void LaserCubeNetManager::discoveryThread() {
    if (!socket) {
        return;
    }

    std::array<std::uint8_t, 64> buffer{};

    while (running.load()) {
        // Broadcast a GET_FULL_INFO probe to discover controllers.
        sendProbe();

        const auto windowStart = Clock::now();
        while (running.load() && Clock::now() - windowStart < std::chrono::seconds(1)) {
            asio::ip::udp::endpoint sender;
            std::size_t received = 0;
            auto ec = socket->recv_from(buffer.data(), buffer.size(), sender, received,
                                        std::chrono::milliseconds(500), false);
            if (ec) {
                if (ec == asio::error::operation_aborted || !running.load()) {
                    break;
                }
                continue;
            }

            // logInfo("[LaserCubeNetManager] discovery rx",
            //         sender.address().to_string(),
            //         sender.port(),
            //         "bytes",
            //         received);

            // Parse and stash the most recent status for each controller.
            if (auto status = LaserCubeNetStatus::parse(buffer.data(), received)) {
                status->ipAddress = sender.address().to_string();
                status->lastSeen = Clock::now();
                bool isNew = false;
                {
                    std::lock_guard lock(controllersMutex);
                    isNew = controllers.find(status->serialNumber) == controllers.end();
                    controllers[status->serialNumber] = ControllerEntry{*status, status->lastSeen};
                }
                if (isNew) {
                    logInfo("[LaserCubeNetManager] discovery ok",
                            status->ipAddress,
                            sender.port(),
                            "serial",
                            status->serialNumber,
                            "model",
                            status->modelName,
                            "fw",
                            status->firmwareVersion,
                            "buffer",
                            status->bufferFree,
                            "/",
                            status->bufferMax,
                            "pps",
                            status->pointRate,
                            "/",
                            status->pointRateMax);
                }
            } else {
                [[maybe_unused]] const auto payloadVersion = received >= 3 ? static_cast<int>(buffer[2]) : -1;
                // logInfo("[LaserCubeNetManager] discovery rx parse failed",
                //         sender.address().to_string(),
                //         sender.port(),
                //         "bytes",
                //         received,
                //         "payloadVersion",
                //         payloadVersion,
                //         "hex",
                //         hexPrefix(buffer.data(), received));
            }
        }

        // Prune stale entries.
        {
            std::lock_guard lock(controllersMutex);
            for (auto it = controllers.begin(); it != controllers.end(); ) {
                if (Clock::now() - it->second.lastSeen > std::chrono::seconds(3)) {
                    it = controllers.erase(it);
                } else {
                    ++it;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void LaserCubeNetManager::sendProbe() {
    if (!socket) return;
    // One-byte command broadcast; controllers reply with a 64-byte status payload.
    const std::uint8_t cmd = LaserCubeNetConfig::CMD_GET_FULL_INFO;
    asio::ip::udp::endpoint broadcastEndpoint(asio::ip::address_v4::broadcast(), LaserCubeNetConfig::COMMAND_PORT);
    socket->send_to(&cmd, 1, broadcastEndpoint, std::chrono::milliseconds(200));
}

std::vector<std::unique_ptr<core::DacInfo>> LaserCubeNetManager::discover() {
    std::vector<std::unique_ptr<core::DacInfo>> out;
    std::lock_guard lock(controllersMutex);
    out.reserve(controllers.size());
    for (const auto& [id, entry] : controllers) {
        out.emplace_back(std::make_unique<LaserCubeNetControllerInfo>(entry.status));
    }
    return out;
}

std::shared_ptr<core::LaserController>
LaserCubeNetManager::getAndConnectToDac(const core::DacInfo& info) {
    const auto* lcInfo = dynamic_cast<const LaserCubeNetControllerInfo*>(&info);
    if (!lcInfo) {
        return nullptr;
    }

    std::shared_ptr<LaserCubeNetController> controller;
    bool newlyCreated = false;
    {
        std::lock_guard lock(activeMutex);
        auto it = active.find(lcInfo->idValue());
        if (it != active.end()) {
            if (auto existing = it->second.lock()) {
                controller = existing;
            } else {
                active.erase(it);
            }
        }

        // Create a new controller if one is not already active.
        if (!controller) {
            controller = std::make_shared<LaserCubeNetController>(*lcInfo);
            active[lcInfo->idValue()] = controller;
            newlyCreated = true;
        }
    }

    if (controller && newlyCreated) {
        // Connect and start the controller thread on first acquisition.
        if (auto result = controller->connect(*lcInfo); !result) {
            logError("[LaserCubeNetManager] initial connect failed", result.error().message());
        }
        controller->start();
    }

    return controller;
}

} // namespace libera::lasercubenet
