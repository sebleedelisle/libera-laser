#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/lasercubenet/LaserCubeNetControllerInfo.hpp"
// ControllerManagerBase has inline lifecycle code that calls controller methods,
// so this manager header needs the full controller type for stricter compilers
// such as MSVC.
#include "libera/lasercubenet/LaserCubeNetController.hpp"
#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/net/NetService.hpp"

#include "libera/core/ThreadUtils.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace libera::lasercubenet {

class LaserCubeNetManager
    : public core::ControllerManagerBase<LaserCubeNetControllerInfo,
                                         LaserCubeNetController> {
public:
    LaserCubeNetManager();
    ~LaserCubeNetManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

    static core::ControllerManagerRegistry registrar;

private:
    using Clock = std::chrono::steady_clock;
    struct ControllerEntry {
        LaserCubeNetStatus status;
        Clock::time_point lastSeen;
    };

    void discoveryThread();
    bool openDiscoverySession();
    void closeDiscoverySession();
    void runDiscoverySession();
    void sendProbe(const std::shared_ptr<net::UdpSocket>& sessionSocket);
    void requestDiscoveryBurst();
    bool waitForNextDiscoveryBurst(std::chrono::steady_clock::duration delay);
    void updateSocketErrorState(const char* action, const std::error_code& ec);

    std::shared_ptr<asio::io_context> io;
    std::shared_ptr<net::UdpSocket> socket;
    std::mutex socketMutex;
    std::thread listener;
    std::atomic<bool> running{false};
    std::atomic<bool> listenerFinished{false};
    std::atomic<bool> discoveryRequested{false};
    std::mutex waitMutex;
    std::condition_variable waitCondition;
    std::optional<std::string> lastSocketError;

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
    core::ControllerManagerRegistration{
        core::ControllerManagerInfo{
            std::string(LaserCubeNetControllerInfo::controllerType()),
            "LaserCube Net",
            "Network LaserCube controllers discovered by broadcast.",
        },
        [] { return std::make_unique<LaserCubeNetManager>(); },
    }
};

} // namespace libera::lasercubenet
