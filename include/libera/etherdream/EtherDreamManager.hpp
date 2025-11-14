#pragma once

#include "libera/core/GlobalDacManager.hpp"
#include "libera/etherdream/EtherDreamDeviceInfo.hpp"
#include "libera/etherdream/EtherDreamDevice.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/log/Log.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace libera::etherdream {

class EtherDreamManager : public core::DacManagerBase {
public:
    EtherDreamManager();
    ~EtherDreamManager() override;

    std::vector<std::unique_ptr<core::DacInfo>> discover() override;
    std::string_view managedType() const override { return typeName; }
    std::shared_ptr<core::LaserDevice> getAndConnectToDac(const core::DacInfo& info) override;
    void closeAll() override;

    static inline core::DacManagerRegistry registrar{
        [] { return std::make_unique<EtherDreamManager>(); }
    };

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::string_view typeName{"etherdream"};

    struct DeviceEntry {
        EtherDreamDeviceInfo info;
        Clock::time_point lastSeen;
    };

    void threadedFunction();
    void pruneStaleUnlocked(Clock::time_point now);

    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<net::UdpSocket> socket;
    std::thread listener;
    std::atomic<bool> running{false};

    std::mutex devicesMutex;
    std::unordered_map<std::string, DeviceEntry> devices;
    std::mutex activeMutex;
    std::unordered_map<std::string, std::weak_ptr<EtherDreamDevice>> activeDevices;
};

} // namespace libera::etherdream
