#include "libera/idn/IdnController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/core/ThreadUtils.hpp"
#include "libera/helios/HeliosTransportSupport.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // Keep Windows headers from defining min/max macros that break std::min/std::max.
#endif
#define _WINSOCKAPI_
#endif
#include "libusb.h"

namespace libera::idn {
namespace {

using HealthProbeResult = IdnController::HealthProbeResult;

std::string describeHeliosError(int code) {
    // Helios SDK error values are shared across USB and IDN. Preserve the same
    // textual mapping here so the log output remains comparable after the
    // transport split.
    if (code <= HELIOS_ERROR_LIBUSB_BASE) {
        const int libusbCode = code - HELIOS_ERROR_LIBUSB_BASE;
        return std::string("libusb:") + libusb_error_name(libusbCode);
    }

    switch (code) {
    case HELIOS_ERROR_DEVICE_CLOSED:
        return "device_closed";
    case HELIOS_ERROR_DEVICE_FRAME_READY:
        return "device_frame_ready";
    case HELIOS_ERROR_DEVICE_SEND_CONTROL:
        return "send_control_failed";
    case HELIOS_ERROR_DEVICE_RESULT:
        return "device_result_unexpected";
    case HELIOS_ERROR_DEVICE_NULL_BUFFER:
        return "null_points";
    case HELIOS_ERROR_DEVICE_SIGNAL_TOO_LONG:
        return "signal_too_long";
    case HELIOS_ERROR_NOT_INITIALIZED:
        return "not_initialized";
    case HELIOS_ERROR_INVALID_DEVNUM:
        return "invalid_device_index";
    case HELIOS_ERROR_NULL_POINTS:
        return "null_points";
    case HELIOS_ERROR_TOO_MANY_POINTS:
        return "too_many_points";
    case HELIOS_ERROR_PPS_TOO_HIGH:
        return "pps_too_high";
    case HELIOS_ERROR_PPS_TOO_LOW:
        return "pps_too_low";
    default:
        return "unknown";
    }
}

struct IdnPingResult {
    HealthProbeResult result = HealthProbeResult::Unknown;
    std::chrono::microseconds roundTrip{0};
};

bool isUsableEndpoint(const IdnController::HealthEndpoint& endpoint) {
    return !endpoint.ip.empty() && endpoint.port != 0;
}

std::chrono::steady_clock::time_point timePointFromTick(
    std::chrono::steady_clock::duration::rep tick) {
    return std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{tick}};
}

std::chrono::steady_clock::duration::rep steadyTickNow() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

HealthProbeResult errorResultForPingFailure(HealthProbeResult result) {
    switch (result) {
        case HealthProbeResult::Timeout:
            return HealthProbeResult::Timeout;
        case HealthProbeResult::ProtocolError:
            return HealthProbeResult::ProtocolError;
        case HealthProbeResult::SendFailed:
        case HealthProbeResult::ReceiveFailed:
        case HealthProbeResult::InvalidEndpoint:
        case HealthProbeResult::SocketError:
        case HealthProbeResult::Unknown:
        case HealthProbeResult::Ok:
            break;
    }

    return HealthProbeResult::SocketError;
}

std::string_view errorTypeForPingFailure(HealthProbeResult result) {
    switch (errorResultForPingFailure(result)) {
        case HealthProbeResult::Timeout:
            return libera::core::error_types::idn::pingTimeout;
        case HealthProbeResult::ProtocolError:
            return libera::core::error_types::idn::pingProtocolError;
        case HealthProbeResult::SendFailed:
        case HealthProbeResult::ReceiveFailed:
        case HealthProbeResult::InvalidEndpoint:
        case HealthProbeResult::SocketError:
        case HealthProbeResult::Unknown:
        case HealthProbeResult::Ok:
            break;
    }

    return libera::core::error_types::idn::pingFailed;
}

bool parseIpv4Address(const std::string& ip, in_addr& address) {
    return inet_pton(AF_INET, ip.c_str(), &address) == 1;
}

timeval timevalFromDuration(std::chrono::steady_clock::duration duration) {
    if (duration < std::chrono::steady_clock::duration::zero()) {
        duration = std::chrono::steady_clock::duration::zero();
    }
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(microseconds / 1000000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>(microseconds % 1000000);
    return tv;
}

IdnPingResult pingEndpoint(const IdnController::HealthEndpoint& endpoint,
                           std::uint16_t sequence,
                           std::chrono::milliseconds timeout) {
    if (!isUsableEndpoint(endpoint)) {
        return {HealthProbeResult::InvalidEndpoint, std::chrono::microseconds{0}};
    }

    sockaddr_in remoteAddress{};
    remoteAddress.sin_family = AF_INET;
    remoteAddress.sin_port = htons(endpoint.port);
    if (!parseIpv4Address(endpoint.ip, remoteAddress.sin_addr)) {
        return {HealthProbeResult::InvalidEndpoint, std::chrono::microseconds{0}};
    }

    const int socketFd = plt_sockOpen(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketFd < 0) {
        return {HealthProbeResult::SocketError, std::chrono::microseconds{0}};
    }

    struct SocketGuard {
        int fd = -1;
        ~SocketGuard() {
            if (fd >= 0) {
                plt_sockClose(fd);
            }
        }
    } socketGuard{socketFd};

    IDNHDR_PACKET request{};
    request.command = IDNCMD_PING_REQUEST;
    request.flags = 0;
    request.sequence = htons(sequence);

    const auto started = std::chrono::steady_clock::now();
    const int sentBytes = sendto(socketFd,
                                 reinterpret_cast<const char*>(&request),
                                 static_cast<int>(sizeof(request)),
                                 0,
                                 reinterpret_cast<const sockaddr*>(&remoteAddress),
                                 sizeof(remoteAddress));
    if (sentBytes != static_cast<int>(sizeof(request))) {
        return {HealthProbeResult::SendFailed, std::chrono::microseconds{0}};
    }

    const auto deadline = started + timeout;
    std::array<std::uint8_t, 256> responseBuffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socketFd, &readSet);

        timeval tv = timevalFromDuration(deadline - std::chrono::steady_clock::now());
        const int ready = select(socketFd + 1, &readSet, nullptr, nullptr, &tv);
        if (ready < 0) {
            return {HealthProbeResult::ReceiveFailed, std::chrono::microseconds{0}};
        }
        if (ready == 0) {
            break;
        }

        sockaddr_in responseAddress{};
        socklen_t responseAddressLength = sizeof(responseAddress);
        const int receivedBytes = recvfrom(
            socketFd,
            reinterpret_cast<char*>(responseBuffer.data()),
            static_cast<int>(responseBuffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&responseAddress),
            &responseAddressLength);
        if (receivedBytes < static_cast<int>(sizeof(IDNHDR_PACKET))) {
            return {HealthProbeResult::ProtocolError, std::chrono::microseconds{0}};
        }
        if (responseAddress.sin_addr.s_addr != remoteAddress.sin_addr.s_addr) {
            continue;
        }

        const auto* response =
            reinterpret_cast<const IDNHDR_PACKET*>(responseBuffer.data());
        if (response->command != IDNCMD_PING_RESPONSE) {
            continue;
        }
        if (ntohs(response->sequence) != sequence) {
            continue;
        }

        return {
            HealthProbeResult::Ok,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started)
        };
    }

    return {HealthProbeResult::Timeout, std::chrono::microseconds{0}};
}

} // namespace

namespace error_types = libera::core::error_types;

IdnController::IdnController(std::shared_ptr<HeliosDac> sdkInstance, unsigned int controllerIndex)
    : IdnController(std::move(sdkInstance), controllerIndex, HealthEndpoint{}) {}

IdnController::IdnController(std::shared_ptr<HeliosDac> sdkInstance,
                             unsigned int controllerIndex,
                             HealthEndpoint endpoint)
    : sdk(std::move(sdkInstance))
    , index(controllerIndex) {
    setHealthEndpoint(std::move(endpoint));
    if (healthEndpointSnapshot()) {
        noteDiscoverySeen();
    }

    const auto defaultFramePoints = helios::detail::defaultFramePointCount(getPointRate());
    targetFramePoints.store(defaultFramePoints, std::memory_order_relaxed);
    frameBuffer.reserve(defaultFramePoints);
    setEstimatedBufferCapacity(static_cast<int>(defaultFramePoints));
    updateEstimatedBufferSnapshotNow(0, getPointRate());
    statusWarmupDeadline = std::chrono::steady_clock::now() + helios::detail::STATUS_ERROR_WARMUP_GRACE;
}

IdnController::~IdnController() {
    stopHealthMonitoring();
    stopThread();
    close();
}

void IdnController::close() {
    stopHealthMonitoring();
    setConnectionState(false);
    clearFrameTransportSubmissionEstimate();
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
    if (sdk) {
        sdk->Stop(index.load(std::memory_order_relaxed));
    }
}

bool IdnController::isConnected() const {
    return sdk && sdk->GetIsClosed(index.load(std::memory_order_relaxed)) == 0;
}

void IdnController::setHealthEndpoint(HealthEndpoint endpoint) {
    std::lock_guard<std::mutex> lock(healthEndpointMutex);
    if (!isUsableEndpoint(endpoint)) {
        healthEndpoint.reset();
        return;
    }
    healthEndpoint = std::move(endpoint);
}

std::optional<IdnController::HealthEndpoint>
IdnController::healthEndpointSnapshot() const {
    std::lock_guard<std::mutex> lock(healthEndpointMutex);
    return healthEndpoint;
}

void IdnController::noteDiscoverySeen() {
    lastDiscoverySeenTick.store(steadyTickNow(), std::memory_order_relaxed);
}

void IdnController::startHealthMonitoring() {
    if (healthRunning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    healthWorkerFinished.store(false, std::memory_order_release);
    healthWorker = std::thread([this] {
        try {
            healthLoop();
        } catch (const std::exception& e) {
            logError("[IdnController] uncaught exception in health worker", e.what());
            recordIntermittentError(error_types::idn::pingFailed);
        } catch (...) {
            logError("[IdnController] uncaught unknown exception in health worker");
            recordIntermittentError(error_types::idn::pingFailed);
        }
        healthWorkerFinished.store(true, std::memory_order_release);
    });
}

void IdnController::stopHealthMonitoring() {
    healthRunning.store(false, std::memory_order_release);
    core::timedJoin(healthWorker,
                    healthWorkerFinished,
                    std::chrono::milliseconds(1000),
                    "IdnController::healthWorker");
}

IdnController::HealthSnapshot IdnController::healthSnapshot() const {
    HealthSnapshot snapshot;
    const auto endpoint = healthEndpointSnapshot();
    if (endpoint) {
        snapshot.endpointKnown = true;
        snapshot.endpoint = *endpoint;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto ageFromTick =
        [now](SteadyRep tick, std::chrono::milliseconds& output) {
            if (tick == 0) {
                return false;
            }
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - timePointFromTick(tick));
            if (age < std::chrono::milliseconds{0}) {
                age = std::chrono::milliseconds{0};
            }
            output = age;
            return true;
        };

    snapshot.pingEverSucceeded =
        pingEverSucceededValue.load(std::memory_order_relaxed);
    snapshot.lastPingResult = static_cast<HealthProbeResult>(
        lastPingResultValue.load(std::memory_order_relaxed));
    snapshot.lastPingAttemptKnown =
        ageFromTick(lastPingAttemptTick.load(std::memory_order_relaxed),
                    snapshot.lastPingAttemptAge);
    snapshot.lastPingSuccessKnown =
        ageFromTick(lastPingSuccessTick.load(std::memory_order_relaxed),
                    snapshot.lastPingSuccessAge);
    snapshot.lastPingRoundTrip = std::chrono::microseconds{
        std::max<std::int64_t>(0, lastPingRoundTripMicros.load(std::memory_order_relaxed))};
    snapshot.consecutivePingFailures =
        consecutivePingFailures.load(std::memory_order_relaxed);
    snapshot.discoverySeen =
        ageFromTick(lastDiscoverySeenTick.load(std::memory_order_relaxed),
                    snapshot.lastDiscoveryAge);
    return snapshot;
}

const char* IdnController::healthProbeResultLabel(HealthProbeResult result) {
    switch (result) {
        case HealthProbeResult::Unknown:
            return "Unknown";
        case HealthProbeResult::Ok:
            return "OK";
        case HealthProbeResult::Timeout:
            return "Timeout";
        case HealthProbeResult::SendFailed:
            return "Send failed";
        case HealthProbeResult::ReceiveFailed:
            return "Receive failed";
        case HealthProbeResult::ProtocolError:
            return "Protocol error";
        case HealthProbeResult::InvalidEndpoint:
            return "Invalid endpoint";
        case HealthProbeResult::SocketError:
            return "Socket error";
    }

    return "Unknown";
}

void IdnController::updateControllerIndex(unsigned int controllerIndex) {
    if (!sdk) {
        return;
    }

    const unsigned int previousIndex =
        index.exchange(controllerIndex, std::memory_order_relaxed);
    if (previousIndex == controllerIndex) {
        return;
    }

    // An IDN reconnect can reassign the stable unit ID to a different transient
    // SDK slot. Flush short-term pacing state so the stream restarts cleanly on
    // the replacement slot instead of carrying stale timing assumptions across.
    consecutiveStatusErrors = 0;
    consecutiveWriteErrors = 0;
    estimatedWriteLeadMicros.store(0, std::memory_order_relaxed);
    clearFrameTransportSubmissionEstimate();
    statusWarmupDeadline =
        std::chrono::steady_clock::now() + helios::detail::STATUS_ERROR_WARMUP_GRACE;
    resetStartupBlank();
}

void IdnController::healthLoop() {
    using namespace std::chrono_literals;

    constexpr auto probeInterval = 2s;
    constexpr auto probeTimeout = 250ms;
    constexpr auto discoveryStaleAfter = 75s;
    constexpr int failureWarningThreshold = 3;
    constexpr auto stopPollInterval = 50ms;

    const auto sleepInterruptibly =
        [this, stopPollInterval](std::chrono::milliseconds duration) {
        std::chrono::milliseconds slept{0};
        while (healthRunning.load(std::memory_order_acquire) && slept < duration) {
            const auto remaining = duration - slept;
            const auto nap = std::min(stopPollInterval, remaining);
            std::this_thread::sleep_for(nap);
            slept += nap;
        }
    };

    while (healthRunning.load(std::memory_order_acquire)) {
        const auto endpoint = healthEndpointSnapshot();
        if (!endpoint) {
            consecutivePingFailures.store(0, std::memory_order_relaxed);
            lastPingResultValue.store(static_cast<int>(HealthProbeResult::InvalidEndpoint),
                                      std::memory_order_relaxed);
            sleepInterruptibly(1s);
            continue;
        }

        const auto attemptTick = steadyTickNow();
        lastPingAttemptTick.store(attemptTick, std::memory_order_relaxed);
        const auto sequence =
            static_cast<std::uint16_t>(nextPingSequence.fetch_add(1, std::memory_order_relaxed));
        const IdnPingResult pingResult = pingEndpoint(*endpoint, sequence, probeTimeout);
        lastPingResultValue.store(static_cast<int>(pingResult.result), std::memory_order_relaxed);

        if (pingResult.result == HealthProbeResult::Ok) {
            consecutivePingFailures.store(0, std::memory_order_relaxed);
            pingEverSucceededValue.store(true, std::memory_order_relaxed);
            lastPingSuccessTick.store(steadyTickNow(), std::memory_order_relaxed);
            lastPingRoundTripMicros.store(pingResult.roundTrip.count(),
                                          std::memory_order_relaxed);
        } else {
            const int failures =
                consecutivePingFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            const bool pingWasEverVerified =
                pingEverSucceededValue.load(std::memory_order_relaxed);
            const SteadyRep lastDiscoveryTick =
                lastDiscoverySeenTick.load(std::memory_order_relaxed);
            const bool discoveryIsStale =
                lastDiscoveryTick != 0 &&
                (std::chrono::steady_clock::now() - timePointFromTick(lastDiscoveryTick)) >=
                    discoveryStaleAfter;

            if (failures >= failureWarningThreshold) {
                if (pingWasEverVerified) {
                    recordIntermittentError(errorTypeForPingFailure(pingResult.result));
                } else if (discoveryIsStale) {
                    recordIntermittentError(error_types::idn::discoveryStale);
                }
            }
        }

        sleepInterruptibly(probeInterval);
    }
}

void IdnController::setPointRate(std::uint32_t pointRateValue) {
    LaserControllerStreaming::setPointRate(pointRateValue);
    if (!framePointCountExplicitlySet.load(std::memory_order_relaxed)) {
        const auto automaticFramePoints =
            helios::detail::defaultFramePointCount(pointRateValue);
        targetFramePoints.store(automaticFramePoints, std::memory_order_relaxed);
        frameBuffer.reserve(automaticFramePoints);
        setEstimatedBufferCapacity(static_cast<int>(automaticFramePoints));
    }
}

void IdnController::setFramePointCount(std::size_t points) {
    const auto clamped = std::clamp<std::size_t>(points,
                                                 helios::detail::MIN_FRAME_POINTS,
                                                 HELIOS_MAX_POINTS);
    framePointCountExplicitlySet.store(true, std::memory_order_relaxed);
    targetFramePoints.store(clamped, std::memory_order_relaxed);
    frameBuffer.reserve(clamped);
    setEstimatedBufferCapacity(static_cast<int>(clamped));
}

std::size_t IdnController::framePointCount() const {
    return targetFramePoints.load(std::memory_order_relaxed);
}

int IdnController::getFirmwareVersion() const {
    if (!sdk) {
        return 0;
    }
    return sdk->GetFirmwareVersion(index.load(std::memory_order_relaxed));
}

std::string IdnController::getDacName() const {
    char buf[32] = {0};
    if (sdk) {
        sdk->GetName(index.load(std::memory_order_relaxed), buf);
    }
    return std::string(buf);
}

bool IdnController::setDacName(const std::string& name) {
    if (!sdk) {
        return false;
    }

    std::string truncated = name.substr(0, 30);
    char buf[32] = {0};
    std::strncpy(buf, truncated.c_str(), 30);
    return sdk->SetName(index.load(std::memory_order_relaxed), buf) == HELIOS_SUCCESS;
}

void IdnController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();
    bool wasConnected = false;

    while (running) {
        const unsigned int sdkIndex = index.load(std::memory_order_relaxed);
        const bool backendConnected = sdk && (sdk->GetIsClosed(sdkIndex) == 0);
        if (!backendConnected) {
            if (wasConnected) {
                recordConnectionError(error_types::usb::connectionLost);
            }
            setConnectionState(false);
            clearFrameTransportSubmissionEstimate();
            wasConnected = false;
            std::this_thread::sleep_for(100ms);
            continue;
        }

        setConnectionState(true);
        if (!wasConnected) {
            resetStartupBlank();
            statusWarmupDeadline =
                std::chrono::steady_clock::now() + helios::detail::STATUS_ERROR_WARMUP_GRACE;
        }
        wasConnected = true;

        const int status = sdk->GetStatus(sdkIndex);
        if (status < 0) {
            if (std::chrono::steady_clock::now() < statusWarmupDeadline) {
                consecutiveStatusErrors = 0;
                std::this_thread::sleep_for(2ms);
                continue;
            }
            if (status == -5007) {
                recordIntermittentError(error_types::usb::timeout);
                std::this_thread::sleep_for(2ms);
                continue;
            }
            recordIntermittentError(error_types::usb::statusError);
            ++consecutiveStatusErrors;
            if (helios::detail::shouldLogErrorBurst(consecutiveStatusErrors)) {
                logError("[IdnController] status error",
                         "index", std::to_string(sdkIndex),
                         "code", status,
                         "reason", describeHeliosError(status),
                         "consecutive", consecutiveStatusErrors);
            }
            std::this_thread::sleep_for(5ms);
            continue;
        }
        statusWarmupDeadline = std::chrono::steady_clock::time_point{};
        consecutiveStatusErrors = 0;

        if (status == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        const std::size_t framePoints = targetFramePoints.load(std::memory_order_relaxed);
        const unsigned int pps = getPointRate();
        const auto writeLead = helios::detail::requestRenderLead(
            std::chrono::microseconds(
                estimatedWriteLeadMicros.load(std::memory_order_relaxed)));
        const auto estimatedFirstRenderTime =
            std::chrono::steady_clock::now() + writeLead;
        const auto pointIndex = currentPointIndex.load(std::memory_order_relaxed);

        setEstimatedBufferCapacity(static_cast<int>(framePoints));
        updateEstimatedBufferSnapshotNow(0, pps);

        // The SDK-backed path still behaves as a point-ingester from the shared
        // scheduler's perspective: pull one controller-sized point batch, then
        // hand that batch to the SDK for packing and submission.
        core::PointFillRequest req;
        req.minimumPointsRequired = framePoints;
        req.maximumPointsRequired = framePoints;
        req.estimatedFirstPointRenderTime = estimatedFirstRenderTime;
        req.currentPointIndex = pointIndex;

        if (!requestPoints(req)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        if (pointsToSend.empty()) {
            continue;
        }

        helios::detail::encodeFramePoints(pointsToSend, frameBuffer);

        const auto sendStart = std::chrono::steady_clock::now();
        const int result = sdk->WriteFrameExtended(sdkIndex,
                                                   pps,
                                                   helios::detail::HELIOS_FLAGS,
                                                   frameBuffer.data(),
                                                   static_cast<unsigned int>(frameBuffer.size()));
        const auto sendDone = std::chrono::steady_clock::now();

        if (result < 0) {
            if (result == -5007) {
                recordIntermittentError(error_types::usb::timeout);
            } else {
                recordIntermittentError(error_types::usb::transferFailed);
            }
            ++consecutiveWriteErrors;
            if (helios::detail::shouldLogErrorBurst(consecutiveWriteErrors)) {
                logError("[IdnController] WriteFrameExtended failed",
                         "index", std::to_string(sdkIndex),
                         "code", result,
                         "reason", describeHeliosError(result),
                         "consecutive", consecutiveWriteErrors,
                         "point_count", frameBuffer.size(),
                         "pps", pps);
            }
            continue;
        }

        consecutiveWriteErrors = 0;
        recordLatencySample(sendDone - sendStart);
        const auto measuredWriteLeadMicros =
            std::chrono::duration_cast<std::chrono::microseconds>(sendDone - sendStart).count();
        const auto previousWriteLeadMicros =
            estimatedWriteLeadMicros.load(std::memory_order_relaxed);
        estimatedWriteLeadMicros.store(
            helios::detail::smoothWriteLeadMicros(previousWriteLeadMicros,
                                                  measuredWriteLeadMicros),
            std::memory_order_relaxed);
        setEstimatedBufferCapacity(static_cast<int>(frameBuffer.size()));
        updateEstimatedBufferSnapshot(static_cast<int>(frameBuffer.size()),
                                     sendDone,
                                     pps);
        currentPointIndex.fetch_add(frameBuffer.size(), std::memory_order_relaxed);
    }
}

} // namespace libera::idn
