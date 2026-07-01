#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/core/ThreadUtils.hpp"
#include "libera/lightspacenet/LightSpaceNetConfig.hpp"
#include "libera/lightspacenet/LightSpaceNetController.hpp"
#include "libera/lightspacenet/LightSpaceNetControllerInfo.hpp"
#include "libera/lightspacenet/LightSpaceNetStatus.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace libera::lightspacenet {

class LightSpaceNetManager
    : public core::ControllerManagerBase<LightSpaceNetControllerInfo,
                                         LightSpaceNetController> {
public:
    LightSpaceNetManager();
    ~LightSpaceNetManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

    static core::ControllerManagerRegistry registrar;

private:
    using Clock = std::chrono::steady_clock;
    struct ControllerEntry {
        LightSpaceNetStatus status;
        Clock::time_point lastSeen;
    };

    void discoveryThread();
    void stopDiscoveryThread();
    bool openDiscoverySession();
    void closeDiscoverySession();
    void runDiscoverySession();
    void sendProbe(const std::shared_ptr<net::UdpSocket>& sessionSocket);
    void requestDiscoveryBurst();
    bool waitForNextDiscoveryBurst(std::chrono::steady_clock::duration delay);
    void pruneStaleControllers();
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

    ControllerPtr createController(const LightSpaceNetControllerInfo& info) override;
    NewControllerDisposition prepareNewController(LightSpaceNetController& controller,
                                                  const LightSpaceNetControllerInfo& info) override;
    void prepareExistingController(LightSpaceNetController& controller,
                                   const LightSpaceNetControllerInfo& info) override;
    void beforeCloseControllers() override;
    void afterCloseControllers() override;
    void closeController(const std::string& key, LightSpaceNetController& controller) override;
};

inline core::ControllerManagerRegistry LightSpaceNetManager::registrar{
    core::ControllerManagerRegistration{
        core::ControllerManagerInfo{
            std::string(LightSpaceNetControllerInfo::controllerType()),
            "LightSpace Net",
            "LS-Net network controllers discovered by LightSpace broadcast.",
        },
        [] { return std::make_unique<LightSpaceNetManager>(); },
    }
};

} // namespace libera::lightspacenet
