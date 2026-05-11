#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubenet/LaserCubeNetController.hpp"

#include "libera/log/Log.hpp"

#include <array>

namespace libera::lasercubenet {

LaserCubeNetManager::LaserCubeNetManager() {
    io = net::shared_io_context();
    socket = std::make_unique<net::UdpSocket>(*io);
    if (!socket->open_v4()) {
        socket->enable_broadcast(true);
        // Bind to the command port so we can receive status replies.
        socket->bind_any(LaserCubeNetConfig::COMMAND_PORT);
    }
    running.store(true);
    listenerFinished.store(false, std::memory_order_relaxed);
    // Dedicated discovery thread so controller scanning never blocks the caller.
    listener = std::thread([this]{
        discoveryThread();
        listenerFinished.store(true, std::memory_order_release);
    });
}

LaserCubeNetManager::~LaserCubeNetManager() {
    closeAll();
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

            // Parse and stash the most recent status for each controller.
            if (auto status = LaserCubeNetStatus::parse(buffer.data(), received)) {
                status->ipAddress = sender.address().to_string();
                status->lastSeen = Clock::now();
                std::shared_ptr<LaserCubeNetController> activeController;
                bool isNew = false;
                {
                    std::lock_guard lock(controllersMutex);
                    isNew = controllers.find(status->serialNumber) == controllers.end();
                    controllers[status->serialNumber] = ControllerEntry{*status, status->lastSeen};
                }
                {
                    activeController = findLiveController(status->serialNumber);
                }
                if (activeController) {
                    activeController->updateDiscoveredStatus(*status);
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

std::vector<std::unique_ptr<core::ControllerInfo>> LaserCubeNetManager::discover() {
    std::vector<std::unique_ptr<core::ControllerInfo>> out;
    std::lock_guard lock(controllersMutex);
    out.reserve(controllers.size());
    for (const auto& [id, entry] : controllers) {
        out.emplace_back(std::make_unique<LaserCubeNetControllerInfo>(entry.status));
    }
    return out;
}

std::shared_ptr<LaserCubeNetController>
LaserCubeNetManager::createController(const LaserCubeNetControllerInfo& info) {
    return std::make_shared<LaserCubeNetController>(info);
}

LaserCubeNetManager::NewControllerDisposition
LaserCubeNetManager::prepareNewController(LaserCubeNetController& controller,
                                          const LaserCubeNetControllerInfo& info) {
    controller.updateDiscoveredStatus(info.status());

    // Connect and start the controller thread on first acquisition.
    if (auto result = controller.connect(info); !result) {
        logError("[LaserCubeNetManager] initial connect failed", result.error().message());
    }
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void LaserCubeNetManager::prepareExistingController(LaserCubeNetController& controller,
                                                    const LaserCubeNetControllerInfo& info) {
    controller.updateDiscoveredStatus(info.status());
}

void LaserCubeNetManager::beforeCloseControllers() {
    running.store(false);
    if (socket) {
        socket->close();
    }
    core::timedJoin(listener, listenerFinished, std::chrono::milliseconds(3000),
                    "LaserCubeNetManager::listener");
}

void LaserCubeNetManager::afterCloseControllers() {
    std::lock_guard lock(controllersMutex);
    controllers.clear();
}

void LaserCubeNetManager::closeController(const std::string& key,
                                          LaserCubeNetController& controller) {
    (void)key;
    controller.close();
}

} // namespace libera::lasercubenet
