#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubenet/LaserCubeNetController.hpp"

#include "libera/log/Log.hpp"

#include <array>
#include <algorithm>

namespace libera::lasercubenet {

LaserCubeNetManager::LaserCubeNetManager() {
    io = net::shared_io_context();
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
    while (running.load()) {
        runDiscoverySession();
        if (!running.load()) {
            break;
        }
        if (!waitForNextDiscoveryBurst(LaserCubeNetConfig::DISCOVERY_IDLE_INTERVAL)) {
            break;
        }
    }
    closeDiscoverySession();
}

bool LaserCubeNetManager::openDiscoverySession() {
    auto sessionSocket = std::make_shared<net::UdpSocket>(*io);
    std::error_code ec;
    if ((ec = sessionSocket->open_v4(false))) {
        updateSocketErrorState("open", ec);
        return false;
    }

    sessionSocket->enable_broadcast(true);
    if ((ec = sessionSocket->bind_any(LaserCubeNetConfig::COMMAND_PORT, false))) {
        updateSocketErrorState("bind", ec);
        return false;
    }

    if (lastSocketError) {
        logInfo("[LaserCubeNetManager] discovery socket recovered");
        lastSocketError.reset();
    }

    {
        std::lock_guard lock(socketMutex);
        socket = std::move(sessionSocket);
    }
    return true;
}

void LaserCubeNetManager::closeDiscoverySession() {
    std::shared_ptr<net::UdpSocket> sessionSocket;
    {
        std::lock_guard lock(socketMutex);
        sessionSocket = std::move(socket);
    }
    if (sessionSocket) {
        sessionSocket->close();
    }
}

void LaserCubeNetManager::runDiscoverySession() {
    if (!openDiscoverySession()) {
        std::lock_guard lock(controllersMutex);
        for (auto it = controllers.begin(); it != controllers.end(); ) {
            if (Clock::now() - it->second.lastSeen > LaserCubeNetConfig::DISCOVERY_STALE_AFTER) {
                it = controllers.erase(it);
            } else {
                ++it;
            }
        }
        return;
    }

    std::shared_ptr<net::UdpSocket> sessionSocket;
    {
        std::lock_guard lock(socketMutex);
        sessionSocket = socket;
    }
    if (!sessionSocket) {
        return;
    }

    std::array<std::uint8_t, 64> buffer{};
    const auto sessionStart = Clock::now();
    const auto deadline = sessionStart + LaserCubeNetConfig::DISCOVERY_LISTEN_WINDOW;
    auto nextProbeAt = sessionStart;

    while (running.load()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }

        if (now >= nextProbeAt) {
            sendProbe(sessionSocket);
            nextProbeAt = now + LaserCubeNetConfig::DISCOVERY_PROBE_INTERVAL;
        }

        const auto nextEventAt = std::min(deadline, nextProbeAt);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextEventAt - now);
        const auto timeout = std::min(LaserCubeNetConfig::DISCOVERY_RECV_TIMEOUT, remaining);

        asio::ip::udp::endpoint sender;
        std::size_t received = 0;
        auto ec = sessionSocket->recv_from(buffer.data(), buffer.size(), sender, received, timeout, false);
        if (ec) {
            if (ec == asio::error::operation_aborted || !running.load()) {
                break;
            }
            if (ec == asio::error::timed_out) {
                continue;
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

    closeDiscoverySession();

    {
        std::lock_guard lock(controllersMutex);
        for (auto it = controllers.begin(); it != controllers.end(); ) {
            if (Clock::now() - it->second.lastSeen > LaserCubeNetConfig::DISCOVERY_STALE_AFTER) {
                it = controllers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void LaserCubeNetManager::sendProbe(const std::shared_ptr<net::UdpSocket>& sessionSocket) {
    if (!sessionSocket) return;
    // One-byte command broadcast; controllers reply with a 64-byte status payload.
    const std::uint8_t cmd = LaserCubeNetConfig::CMD_GET_FULL_INFO;
    asio::ip::udp::endpoint broadcastEndpoint(asio::ip::address_v4::broadcast(), LaserCubeNetConfig::COMMAND_PORT);
    sessionSocket->send_to(&cmd, 1, broadcastEndpoint, std::chrono::milliseconds(200));
}

bool LaserCubeNetManager::waitForNextDiscoveryBurst(std::chrono::steady_clock::duration delay) {
    std::unique_lock lock(waitMutex);
    waitCondition.wait_for(lock, delay, [this] {
        return !running.load() || discoveryRequested.load(std::memory_order_relaxed);
    });
    if (!running.load()) {
        return false;
    }
    discoveryRequested.store(false, std::memory_order_relaxed);
    return true;
}

void LaserCubeNetManager::updateSocketErrorState(const char* action,
                                                 const std::error_code& ec) {
    if (!ec) {
        return;
    }

    const std::string message = std::string(action) + " failed: " + ec.message();
    if (!lastSocketError || *lastSocketError != message) {
        logError("[LaserCubeNetManager] discovery socket", message);
        lastSocketError = message;
    }
}

std::vector<std::unique_ptr<core::ControllerInfo>> LaserCubeNetManager::discover() {
    requestDiscoveryBurst();
    std::vector<std::unique_ptr<core::ControllerInfo>> out;
    std::lock_guard lock(controllersMutex);
    out.reserve(controllers.size());
    for (const auto& [id, entry] : controllers) {
        out.emplace_back(std::make_unique<LaserCubeNetControllerInfo>(entry.status));
    }
    return out;
}

void LaserCubeNetManager::requestDiscoveryBurst() {
    discoveryRequested.store(true, std::memory_order_relaxed);
    waitCondition.notify_all();
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
    waitCondition.notify_all();
    closeDiscoverySession();
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
