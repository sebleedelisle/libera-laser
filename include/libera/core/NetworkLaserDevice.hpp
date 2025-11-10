#pragma once

#include "libera/core/LaserDeviceBase.hpp"
#include "libera/core/Expected.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace libera::core {

/**
 * @brief Laser device base that provides automatic reconnection support.
 */
class NetworkLaserDevice : public LaserDeviceBase {
public:
    NetworkLaserDevice();
    ~NetworkLaserDevice() override;

    void enableAutoReconnect(bool enable);

protected:
    /// Derived classes must indicate if they have enough info to attempt a reconnect.
    virtual bool hasConnectionInfo() const = 0;
    /// Derived classes must perform the actual reconnect attempt using stored info.
    virtual libera::expected<void> connectUsingStoredInfo() = 0;
    /// Optional hook invoked after a successful reconnect.
    virtual void onReconnectSucceeded() {}
    /// Optional hook invoked after a failed attempt.
    virtual void onReconnectFailed(const std::error_code&) {}

    void scheduleReconnect();

    void startReconnectSupervisor();
    void stopReconnectSupervisor();

private:
    void reconnectThreadMain();

    std::thread reconnectThread;
    std::mutex reconnectMutex;
    std::condition_variable reconnectCv;
    std::atomic<bool> reconnectThreadRunning{false};
    bool reconnectStopRequested = false;
    bool reconnectPending = false;
    std::atomic<bool> autoReconnectEnabled{true};
};

} // namespace libera::core
