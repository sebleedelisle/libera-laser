#pragma once

#include "libera/core/GlobalDacManager.hpp"
#include "libera/lasercubenet/LaserCubeNetControllerInfo.hpp"
#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/net/NetService.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace libera::lasercubenet {

class LaserCubeNetController;

class LaserCubeNetManager : public core::DacManagerBase {
public:
    LaserCubeNetManager();
    ~LaserCubeNetManager() override;

    std::vector<std::unique_ptr<core::DacInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> getAndConnectToDac(const core::DacInfo& info) override;
    void closeAll() override;

    static inline core::DacManagerRegistry registrar{
        [] { return std::make_unique<LaserCubeNetManager>(); }
    };

private:
    using Clock = std::chrono::steady_clock;
    struct ControllerEntry {
        LaserCubeNetStatus status;
        Clock::time_point lastSeen;
    };

    void discoveryThread();
    void sendProbe();
    void processResponses();

    static constexpr std::string_view typeName{"LaserCubeNet"};

    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<net::UdpSocket> socket;
    std::thread listener;
    std::atomic<bool> running{false};

    std::mutex controllersMutex;
    std::unordered_map<std::string, ControllerEntry> controllers;
    std::mutex activeMutex;
    std::unordered_map<std::string, std::weak_ptr<LaserCubeNetController>> active;
};

} // namespace libera::lasercubenet
