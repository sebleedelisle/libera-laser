#pragma once

#include "libera/core/DacDiscovery.hpp"
#include "libera/etherdream/EtherDreamDeviceInfo.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace libera::etherdream {

class EtherDreamDiscoverer : public core::DacDiscovererBase {
public:
    EtherDreamDiscoverer();
    ~EtherDreamDiscoverer() override;

    std::vector<std::unique_ptr<core::DiscoveredDac>> discover() override;

    static inline core::DiscovererRegistry registrar{
        [] { return std::make_unique<EtherDreamDiscoverer>(); }
    };

private:
    using Clock = std::chrono::steady_clock;

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
};

} // namespace libera::etherdream
