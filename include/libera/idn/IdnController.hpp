#pragma once

#include "libera/core/LaserController.hpp"
#include "../../../libs/helios_dac/sdk/cpp/HeliosDac.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace libera::idn {

class IdnController : public core::LaserController {
public:
    explicit IdnController(std::shared_ptr<HeliosDac> sdk, unsigned int controllerIndex);
    struct HealthEndpoint {
        std::string ip;
        std::uint16_t port = 0;
    };

    enum class HealthProbeResult {
        Unknown,
        Ok,
        Timeout,
        SendFailed,
        ReceiveFailed,
        ProtocolError,
        InvalidEndpoint,
        SocketError
    };

    struct HealthSnapshot {
        bool endpointKnown = false;
        HealthEndpoint endpoint{};
        bool pingEverSucceeded = false;
        HealthProbeResult lastPingResult = HealthProbeResult::Unknown;
        bool lastPingAttemptKnown = false;
        std::chrono::milliseconds lastPingAttemptAge{0};
        bool lastPingSuccessKnown = false;
        std::chrono::milliseconds lastPingSuccessAge{0};
        std::chrono::microseconds lastPingRoundTrip{0};
        int consecutivePingFailures = 0;
        bool discoverySeen = false;
        std::chrono::milliseconds lastDiscoveryAge{0};
    };

    IdnController(std::shared_ptr<HeliosDac> sdk,
                  unsigned int controllerIndex,
                  HealthEndpoint endpoint);
    ~IdnController() override;

    void close();
    bool isConnected() const;
    void updateControllerIndex(unsigned int controllerIndex);
    unsigned int controllerIndex() const { return index.load(std::memory_order_relaxed); }
    void setHealthEndpoint(HealthEndpoint endpoint);
    void noteDiscoverySeen();
    void startHealthMonitoring();
    void stopHealthMonitoring();
    HealthSnapshot healthSnapshot() const;
    static const char* healthProbeResultLabel(HealthProbeResult result);

    void setPointRate(std::uint32_t pointRateValue) override;

    void setFramePointCount(std::size_t points);
    std::size_t framePointCount() const;

    /// Returns the firmware version reported by the SDK-backed IDN device.
    int getFirmwareVersion() const;

    /// Returns the current DAC name as exposed by the Helios SDK.
    std::string getDacName() const;

    /// Writes a new DAC name through the Helios SDK (max 30 characters).
    bool setDacName(const std::string& name);

protected:
    void run() override;

private:
    std::shared_ptr<HeliosDac> sdk;
    std::atomic<unsigned int> index{0};
    std::atomic<std::size_t> targetFramePoints{1000};
    std::atomic<bool> framePointCountExplicitlySet{false};
    std::atomic<std::uint64_t> currentPointIndex{0};
    std::atomic<std::int64_t> estimatedWriteLeadMicros{0};
    std::vector<HeliosPointExt> frameBuffer;

    // These counters stay on the controller so reconnects can clear the noisy
    // short-term history without touching longer-lived manager state.
    std::size_t consecutiveStatusErrors = 0;
    std::size_t consecutiveWriteErrors = 0;
    std::chrono::steady_clock::time_point statusWarmupDeadline{};

    using SteadyRep = std::chrono::steady_clock::duration::rep;

    std::optional<HealthEndpoint> healthEndpoint;
    mutable std::mutex healthEndpointMutex;
    std::thread healthWorker;
    std::atomic<bool> healthRunning{false};
    std::atomic<bool> healthWorkerFinished{true};
    std::atomic<unsigned int> nextPingSequence{1};
    std::atomic<SteadyRep> lastDiscoverySeenTick{0};
    std::atomic<SteadyRep> lastPingAttemptTick{0};
    std::atomic<SteadyRep> lastPingSuccessTick{0};
    std::atomic<std::int64_t> lastPingRoundTripMicros{0};
    std::atomic<int> lastPingResultValue{static_cast<int>(HealthProbeResult::Unknown)};
    std::atomic<int> consecutivePingFailures{0};
    std::atomic<bool> pingEverSucceededValue{false};

    std::optional<HealthEndpoint> healthEndpointSnapshot() const;
    void healthLoop();
};

} // namespace libera::idn
