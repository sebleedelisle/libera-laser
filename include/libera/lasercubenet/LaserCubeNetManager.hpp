#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/lasercubenet/LaserCubeNetControllerInfo.hpp"
#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/net/NetService.hpp"

#include "libera/core/ThreadUtils.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace libera::lasercubenet {

class LaserCubeNetController;

class LaserCubeNetManager
    : public core::ControllerManagerBase<LaserCubeNetControllerInfo,
                                         LaserCubeNetController> {
public:
    LaserCubeNetManager();
    ~LaserCubeNetManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }

    static core::ControllerManagerRegistry registrar;

private:
    using Clock = std::chrono::steady_clock;
    struct ControllerEntry {
        LaserCubeNetStatus status;
        Clock::time_point lastSeen;
    };

    void discoveryThread();
    void sendProbe();

    static constexpr std::string_view typeName{"LaserCubeNet"};

    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<net::UdpSocket> socket;
    std::thread listener;
    std::atomic<bool> running{false};
    std::atomic<bool> listenerFinished{false};

    std::mutex controllersMutex;
    std::unordered_map<std::string, ControllerEntry> controllers;

    ControllerPtr createController(const LaserCubeNetControllerInfo& info) override;
    NewControllerDisposition prepareNewController(LaserCubeNetController& controller,
                                                  const LaserCubeNetControllerInfo& info) override;
    void prepareExistingController(LaserCubeNetController& controller,
                                   const LaserCubeNetControllerInfo& info) override;
    void beforeCloseControllers() override;
    void afterCloseControllers() override;
    void closeController(const std::string& key, LaserCubeNetController& controller) override;
};

inline core::ControllerManagerRegistry LaserCubeNetManager::registrar{
    [] { return std::make_unique<LaserCubeNetManager>(); }
};

} // namespace libera::lasercubenet
