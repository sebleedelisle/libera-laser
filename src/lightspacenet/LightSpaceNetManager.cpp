#include "libera/lightspacenet/LightSpaceNetManager.hpp"

#include "libera/lightspacenet/LightSpaceNetPacket.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>

namespace libera::lightspacenet {

LightSpaceNetManager::LightSpaceNetManager() {
    io = net::shared_io_context();
    running.store(true);
    listenerFinished.store(false, std::memory_order_relaxed);
    listener = std::thread([this] {
        discoveryThread();
        listenerFinished.store(true, std::memory_order_release);
    });
}

LightSpaceNetManager::~LightSpaceNetManager() {
    closeAll();
}

void LightSpaceNetManager::stopDiscoveryThread() {
    running.store(false);
    waitCondition.notify_all();
    closeDiscoverySession();
    core::timedJoin(listener, listenerFinished, std::chrono::milliseconds(3000),
                    "LightSpaceNetManager::listener");
}

void LightSpaceNetManager::discoveryThread() {
    while (running.load()) {
        runDiscoverySession();
        if (!running.load()) {
            break;
        }
        if (!waitForNextDiscoveryBurst(LightSpaceNetConfig::DISCOVERY_IDLE_INTERVAL)) {
            break;
        }
    }
    closeDiscoverySession();
}

bool LightSpaceNetManager::openDiscoverySession() {
    auto sessionSocket = std::make_shared<net::UdpSocket>(*io);
    std::error_code ec;
    if ((ec = sessionSocket->open_v4(false))) {
        updateSocketErrorState("open", ec);
        return false;
    }
    sessionSocket->enable_broadcast(true);

    // The LS-Net document says UDP communication uses port 25555. Binding the
    // short-lived discovery socket there lets devices reply to the documented
    // host port without keeping it claimed between discovery bursts.
    if ((ec = sessionSocket->bind_any(LightSpaceNetConfig::NETWORK_PORT, false))) {
        updateSocketErrorState("bind", ec);
        return false;
    }

    if (lastSocketError) {
        logInfo("[LightSpaceNetManager] discovery socket recovered");
        lastSocketError.reset();
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex);
        socket = std::move(sessionSocket);
    }
    return true;
}

void LightSpaceNetManager::closeDiscoverySession() {
    std::shared_ptr<net::UdpSocket> sessionSocket;
    {
        std::lock_guard<std::mutex> lock(socketMutex);
        sessionSocket = std::move(socket);
    }
    if (sessionSocket) {
        sessionSocket->close();
    }
}

void LightSpaceNetManager::runDiscoverySession() {
    if (!openDiscoverySession()) {
        pruneStaleControllers();
        return;
    }

    std::shared_ptr<net::UdpSocket> sessionSocket;
    {
        std::lock_guard<std::mutex> lock(socketMutex);
        sessionSocket = socket;
    }
    if (!sessionSocket) {
        return;
    }

    std::array<std::uint8_t, 2048> buffer{};
    const auto sessionStart = Clock::now();
    const auto deadline = sessionStart + LightSpaceNetConfig::DISCOVERY_LISTEN_WINDOW;
    auto nextProbeAt = sessionStart;

    while (running.load()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }

        if (now >= nextProbeAt) {
            sendProbe(sessionSocket);
            nextProbeAt = now + LightSpaceNetConfig::DISCOVERY_PROBE_INTERVAL;
        }

        const auto nextEventAt = std::min(deadline, nextProbeAt);
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(nextEventAt - now);
        const auto timeout =
            std::min(LightSpaceNetConfig::DISCOVERY_RECV_TIMEOUT, remaining);

        net::udp::endpoint sender;
        std::size_t received = 0;
        auto ec = sessionSocket->recv_from(buffer.data(), buffer.size(), sender,
                                           received, timeout, false);
        if (ec) {
            if (ec == net::asio::error::operation_aborted || !running.load()) {
                break;
            }
            if (ec == net::asio::error::timed_out) {
                continue;
            }
            continue;
        }

        auto status = LightSpaceNetStatus::parseBroadcastResponse(buffer.data(), received);
        if (!status) {
            continue;
        }

        // The response payload contains an IP address, but the sender address
        // is the route that actually reached us on this interface.
        status->ipAddress = sender.address().to_string();
        status->lastSeen = Clock::now();
        const auto id = status->stableId();
        std::shared_ptr<LightSpaceNetController> activeController;
        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(controllersMutex);
            isNew = controllers.find(id) == controllers.end();
            controllers[id] = ControllerEntry{*status, status->lastSeen};
        }
        activeController = findLiveController(id);
        if (activeController) {
            activeController->updateDiscoveredStatus(*status);
        }

        if (isNew) {
            logInfo("[LightSpaceNetManager] discovery ok",
                    status->ipAddress,
                    "id",
                    id,
                    "name",
                    status->displayLabel(),
                    "fw",
                    status->firmwareVersion,
                    "hw",
                    status->hardwareVersion);
        }
    }

    closeDiscoverySession();
    pruneStaleControllers();
}

void LightSpaceNetManager::sendProbe(const std::shared_ptr<net::UdpSocket>& sessionSocket) {
    if (!sessionSocket) {
        return;
    }
    const auto packet = buildBroadcastQueryPacket();
    net::udp::endpoint broadcastEndpoint(
        net::asio::ip::address_v4::broadcast(),
        LightSpaceNetConfig::NETWORK_PORT);
    sessionSocket->send_to(packet.data(), packet.size(), broadcastEndpoint,
                           std::chrono::milliseconds(200));
}

bool LightSpaceNetManager::waitForNextDiscoveryBurst(
    std::chrono::steady_clock::duration delay) {
    std::unique_lock<std::mutex> lock(waitMutex);
    waitCondition.wait_for(lock, delay, [this] {
        return !running.load() || discoveryRequested.load(std::memory_order_relaxed);
    });
    if (!running.load()) {
        return false;
    }
    discoveryRequested.store(false, std::memory_order_relaxed);
    return true;
}

void LightSpaceNetManager::pruneStaleControllers() {
    std::lock_guard<std::mutex> lock(controllersMutex);
    for (auto it = controllers.begin(); it != controllers.end(); ) {
        if (Clock::now() - it->second.lastSeen > LightSpaceNetConfig::DISCOVERY_STALE_AFTER) {
            it = controllers.erase(it);
        } else {
            ++it;
        }
    }
}

void LightSpaceNetManager::updateSocketErrorState(const char* action,
                                                  const std::error_code& ec) {
    if (!ec) {
        return;
    }
    const std::string message = std::string(action) + " failed: " + ec.message();
    if (!lastSocketError || *lastSocketError != message) {
        logError("[LightSpaceNetManager] discovery socket", message);
        lastSocketError = message;
    }
}

std::vector<std::unique_ptr<core::ControllerInfo>> LightSpaceNetManager::discover() {
    requestDiscoveryBurst();
    std::vector<std::unique_ptr<core::ControllerInfo>> out;
    std::lock_guard<std::mutex> lock(controllersMutex);
    out.reserve(controllers.size());
    for (const auto& [id, entry] : controllers) {
        (void)id;
        out.emplace_back(std::make_unique<LightSpaceNetControllerInfo>(entry.status));
    }
    return out;
}

void LightSpaceNetManager::requestDiscoveryBurst() {
    discoveryRequested.store(true, std::memory_order_relaxed);
    waitCondition.notify_all();
}

std::shared_ptr<LightSpaceNetController>
LightSpaceNetManager::createController(const LightSpaceNetControllerInfo& info) {
    return std::make_shared<LightSpaceNetController>(info);
}

LightSpaceNetManager::NewControllerDisposition
LightSpaceNetManager::prepareNewController(LightSpaceNetController& controller,
                                           const LightSpaceNetControllerInfo& info) {
    // Once a controller is selected, stop discovery. Active playback uses TCP,
    // but repeated UDP discovery probes can still steal device attention on
    // some LS-Net firmware while the TCP pattern stream is running.
    stopDiscoveryThread();
    if (info.preferredPointRate() > 0) {
        static_cast<core::LaserController&>(controller).setPointRate(info.preferredPointRate());
    }
    controller.updateDiscoveredStatus(info.status());
    if (auto result = controller.connect(info); !result) {
        logError("[LightSpaceNetManager] initial connect failed", result.error().message());
    }
    controller.startThread();
    return NewControllerDisposition::KeepController;
}

void LightSpaceNetManager::prepareExistingController(LightSpaceNetController& controller,
                                                     const LightSpaceNetControllerInfo& info) {
    if (info.preferredPointRate() > 0) {
        static_cast<core::LaserController&>(controller).setPointRate(info.preferredPointRate());
    }
    controller.updateDiscoveredStatus(info.status());
}

void LightSpaceNetManager::beforeCloseControllers() {
    stopDiscoveryThread();
}

void LightSpaceNetManager::afterCloseControllers() {
    std::lock_guard<std::mutex> lock(controllersMutex);
    controllers.clear();
}

void LightSpaceNetManager::closeController(const std::string& key,
                                           LightSpaceNetController& controller) {
    (void)key;
    controller.close();
}

} // namespace libera::lightspacenet
