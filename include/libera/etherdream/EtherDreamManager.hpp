#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/etherdream/EtherDreamControllerInfo.hpp"
#include "libera/etherdream/EtherDreamController.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/log/Log.hpp"

#include "libera/core/ThreadUtils.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace libera::etherdream {

class EtherDreamManager
    : public core::ControllerManagerBase<EtherDreamControllerInfo,
                                         EtherDreamController> {
public:
    EtherDreamManager();
    ~EtherDreamManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

    static core::ControllerManagerRegistry registrar;

private:
    using Clock = std::chrono::steady_clock;
    struct ControllerEntry {
        EtherDreamControllerInfo info;
        Clock::time_point lastSeen;
    };

    void threadedFunction();
    bool openDiscoverySession();
    void closeDiscoverySession();
    void runDiscoverySession();
    void requestDiscoveryBurst();
    bool waitForNextDiscoveryBurst(std::chrono::steady_clock::duration delay);
    void pruneStaleUnlocked(Clock::time_point now);
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

    ControllerPtr createController(const EtherDreamControllerInfo& info) override;
    NewControllerDisposition prepareNewController(EtherDreamController& controller,
                                                  const EtherDreamControllerInfo& info) override;
    void beforeCloseControllers() override;
    void afterCloseControllers() override;
    void closeController(const std::string& key, EtherDreamController& controller) override;
};

inline core::ControllerManagerRegistry EtherDreamManager::registrar{
    core::ControllerManagerRegistration{
        core::ControllerManagerInfo{
            std::string(EtherDreamControllerInfo::controllerType()),
            "Ether Dream",
            "Network DACs discovered from Ether Dream broadcasts.",
        },
        [] { return std::make_unique<EtherDreamManager>(); },
    }
};

} // namespace libera::etherdream
