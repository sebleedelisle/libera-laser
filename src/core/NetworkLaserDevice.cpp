#include "libera/core/NetworkLaserDevice.hpp"

namespace libera::core {

NetworkLaserDevice::NetworkLaserDevice() = default;

NetworkLaserDevice::~NetworkLaserDevice() {
    stopReconnectSupervisor();
}

void NetworkLaserDevice::enableAutoReconnect(bool enable) {
    autoReconnectEnabled.store(enable);
    if (enable) {
        scheduleReconnect();
    }
}

void NetworkLaserDevice::startReconnectSupervisor() {
    if (reconnectThreadRunning.load()) {
        return;
    }
    reconnectStopRequested = false;
    reconnectThreadRunning.store(true);
    reconnectThread = std::thread([this] { reconnectThreadMain(); });
}

void NetworkLaserDevice::stopReconnectSupervisor() {
    reconnectStopRequested = true;
    reconnectCv.notify_all();
    if (reconnectThread.joinable()) {
        reconnectThread.join();
    }
    reconnectThreadRunning.store(false);
}

void NetworkLaserDevice::scheduleReconnect() {
    if (!autoReconnectEnabled.load()) {
        return;
    }
    {
        std::lock_guard lock(reconnectMutex);
        reconnectPending = true;
    }
    reconnectCv.notify_one();
}

void NetworkLaserDevice::reconnectThreadMain() {
    using namespace std::chrono;
    const milliseconds initialDelay{100};

    while (true) {
        std::unique_lock lock(reconnectMutex);
        reconnectCv.wait(lock, [&] { return reconnectPending || reconnectStopRequested; });
        if (reconnectStopRequested) {
            break;
        }
        reconnectPending = false;
        lock.unlock();

        if (!autoReconnectEnabled.load() || !hasConnectionInfo()) {
            continue;
        }

        stop();

        const auto failureStart = steady_clock::now();
        auto delay = initialDelay;

        while (!reconnectStopRequested && autoReconnectEnabled.load()) {
            if (!hasConnectionInfo()) {
                break;
            }

            auto result = connectUsingStoredInfo();
            if (result) {
                onReconnectSucceeded();
                start();
                break;
            }

            onReconnectFailed(result.error());

            if (steady_clock::now() - failureStart > minutes(1)) {
                delay = seconds(1);
            }

            std::unique_lock waitLock(reconnectMutex);
            reconnectCv.wait_for(waitLock, delay, [&] { return reconnectPending || reconnectStopRequested; });
            if (reconnectStopRequested) {
                break;
            }
            if (reconnectPending) {
                reconnectPending = false;
            }
        }
    }
}

} // namespace libera::core
