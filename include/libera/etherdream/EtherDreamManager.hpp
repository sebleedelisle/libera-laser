#pragma once

#include "libera/System.hpp"
#include "libera/core/ControllerCache.hpp"
#include "libera/etherdream/EtherDreamControllerInfo.hpp"
#include "libera/etherdream/EtherDreamController.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/log/Log.hpp"

#include "libera/core/ThreadUtils.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace libera::etherdream {

class EtherDreamManager : public core::ControllerManagerBase {
public:
    EtherDreamManager();
    ~EtherDreamManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info) override;
    void closeAll() override;

    static core::ControllerManagerRegistry registrar;

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::string_view typeName{"EtherDream"};

    struct ControllerEntry {
        EtherDreamControllerInfo info;
        Clock::time_point lastSeen;
    };

    void threadedFunction();
    void pruneStaleUnlocked(Clock::time_point now);

    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<net::UdpSocket> socket;
    std::thread listener;
    std::atomic<bool> running{false};
    std::atomic<bool> listenerFinished{false};

    std::mutex controllersMutex;
    std::unordered_map<std::string, ControllerEntry> controllers;
    core::ControllerCache<std::string, EtherDreamController> activeControllers;
};

inline core::ControllerManagerRegistry EtherDreamManager::registrar{
    [] { return std::make_unique<EtherDreamManager>(); }
};

} // namespace libera::etherdream
