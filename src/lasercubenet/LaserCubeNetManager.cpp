#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubenet/LaserCubeNetDevice.hpp"

#include "libera/log/Log.hpp"

#include <array>
#include <iomanip>
#include <sstream>

namespace libera::lasercubenet {
namespace {
std::string hexPrefix(const std::uint8_t* data, std::size_t size, std::size_t maxBytes = 16) {
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
        socket->bind_any(LaserCubeNetConfig::COMMAND_PORT);
    }
    running.store(true);
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

    std::unordered_map<std::string, std::shared_ptr<LaserCubeNetDevice>> snapshot;
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

    std::lock_guard lock(devicesMutex);
    devices.clear();
}

void LaserCubeNetManager::discoveryThread() {
    if (!socket) {
        return;
    }

    asio::ip::udp::endpoint broadcastEndpoint(asio::ip::address_v4::broadcast(), LaserCubeNetConfig::COMMAND_PORT);
    std::array<std::uint8_t, 64> buffer{};

    while (running.load()) {
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

            logInfo("[LaserCubeNetManager] discovery rx",
                    sender.address().to_string(),
                    sender.port(),
                    "bytes",
                    received);

            if (auto status = LaserCubeNetStatus::parse(buffer.data(), received)) {
                status->ipAddress = sender.address().to_string();
                status->lastSeen = Clock::now();
                bool isNew = false;
                {
                    std::lock_guard lock(devicesMutex);
                    isNew = devices.find(status->serialNumber) == devices.end();
                    devices[status->serialNumber] = DeviceEntry{*status, status->lastSeen};
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
                const auto payloadVersion = received >= 3 ? static_cast<int>(buffer[2]) : -1;
                logInfo("[LaserCubeNetManager] discovery rx parse failed",
                        sender.address().to_string(),
                        sender.port(),
                        "bytes",
                        received,
                        "payloadVersion",
                        payloadVersion,
                        "hex",
                        hexPrefix(buffer.data(), received));
            }
        }

        // Prune stale entries
        {
            std::lock_guard lock(devicesMutex);
            for (auto it = devices.begin(); it != devices.end(); ) {
                if (Clock::now() - it->second.lastSeen > std::chrono::seconds(3)) {
                    it = devices.erase(it);
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
    const std::uint8_t cmd = LaserCubeNetConfig::CMD_GET_FULL_INFO;
    asio::ip::udp::endpoint broadcastEndpoint(asio::ip::address_v4::broadcast(), LaserCubeNetConfig::COMMAND_PORT);
    socket->send_to(&cmd, 1, broadcastEndpoint, std::chrono::milliseconds(200));
}

std::vector<std::unique_ptr<core::DacInfo>> LaserCubeNetManager::discover() {
    std::vector<std::unique_ptr<core::DacInfo>> out;
    std::lock_guard lock(devicesMutex);
    out.reserve(devices.size());
    for (const auto& [id, entry] : devices) {
        out.emplace_back(std::make_unique<LaserCubeNetDeviceInfo>(entry.status));
    }
    return out;
}

std::shared_ptr<core::LaserDevice>
LaserCubeNetManager::getAndConnectToDac(const core::DacInfo& info) {
    const auto* lcInfo = dynamic_cast<const LaserCubeNetDeviceInfo*>(&info);
    if (!lcInfo) {
        return nullptr;
    }

    std::shared_ptr<LaserCubeNetDevice> device;
    bool newlyCreated = false;
    {
        std::lock_guard lock(activeMutex);
        auto it = active.find(lcInfo->idValue());
        if (it != active.end()) {
            if (auto existing = it->second.lock()) {
                device = existing;
            } else {
                active.erase(it);
            }
        }

        if (!device) {
            device = std::make_shared<LaserCubeNetDevice>(*lcInfo);
            active[lcInfo->idValue()] = device;
            newlyCreated = true;
        }
    }

    if (device && newlyCreated) {
        if (auto result = device->connect(*lcInfo); !result) {
            logError("[LaserCubeNetManager] initial connect failed", result.error().message());
        }
        device->start();
    }

    return device;
}

} // namespace libera::lasercubenet
