/**
 * @brief Implements the EtherDream DAC worker loop: connection, polling, and streaming.
 */
#include "libera/etherdream/EtherDreamController.hpp"

#include "libera/core/BufferEstimator.hpp"
#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string_view>
#include <system_error>
#include <thread>
#include <cstddef>  // for std::byte, std::to_integer
#include <cstdint>
#include <cctype>
#include <mutex>

using namespace std::chrono_literals; // Enable 100ms / 1s literals.

namespace libera::etherdream {

using libera::expected;
using libera::unexpected;
using Ack = EtherDreamController::Ack;
namespace ip = libera::net::asio::ip;
namespace asio = libera::net::asio;
namespace error_types = libera::core::error_types;

EtherDreamController::EtherDreamController() = default;

EtherDreamController::EtherDreamController(EtherDreamControllerInfo info)
: controllerInfo(std::move(info)) {}

EtherDreamController::~EtherDreamController() {
    // Orderly shutdown: stop the worker thread and close the TCP connection.
    stop();
    close();
}

expected<void> EtherDreamController::connect(const EtherDreamControllerInfo& info) {
    controllerInfo = info;
    return connect();
}

expected<void> EtherDreamController::connect() {
    if (!controllerInfo) {
        lastError = std::make_error_code(std::errc::invalid_argument);
        recordConnectionError(error_types::network::connectFailed);
        return unexpected(*lastError);
    }

    std::error_code ec;
    auto ip = libera::net::asio::ip::make_address(controllerInfo->ip(), ec);
    if (ec) {
        logError("Invalid IP", ec.message());
        lastError = ec;
        recordConnectionError(error_types::network::connectFailed);
        return unexpected(ec);
    }

    libera::net::tcp::endpoint endpoint(ip, controllerInfo->port());

    if (auto connectError = tcpClient.connect(endpoint); connectError) {
        logError("[EtherDreamController] connect failed", connectError.message(),
                 "target", controllerInfo->ip(), controllerInfo->port(),
                 "timeout_ms", tcpClient.defaultTimeout().count());
        lastError = connectError;
        recordConnectionError(error_types::network::connectFailed);
        return unexpected(connectError);
    }
    // Ask the socket to send small packets right away and keep the connection alive.
    tcpClient.setLowLatency();

    tcpClient.setDefaultTimeout(200ms);
    tcpClient.setConnectTimeout(1s);


    logInfoVerbose("[EtherDreamController] connected to", controllerInfo->ip(), controllerInfo->port());
    lastKnownBufferCapacity.store(getBufferSize(), std::memory_order_relaxed);
    clearNetworkError();
    setConnectionState(true);

    return {};
}

void EtherDreamController::run() {
    constexpr auto retryDelay = std::chrono::milliseconds(100);

    while (running) {
        if (!controllerInfo) {
            setConnectionState(false);
            std::this_thread::sleep_for(retryDelay);
            continue;
        }

        if (!ensureConnected()) {
            std::this_thread::sleep_for(retryDelay);
            continue;
        }

        connectionActive = true;
        setConnectionState(true);

        if (!performHandshake()) {
            close();
            std::this_thread::sleep_for(retryDelay);
            continue;
        }
        // make sure to blank points if reconnecting
        resetStartupBlank(); 

        while (running && connectionActive) {
            if (clearRequired) {
                sendClear();
            }

            if (prepareRequired) {
                sendPrepare();
            }

            if (auto newrate = nextPendingRateChange()) {
                if (auto rateAck = sendPointRate(*newrate); !rateAck) {
                    handleNetworkFailure("point rate command", rateAck.error());
                }
            }

            sleepUntilNextPoints();

            auto req = getFillRequest();
            if (req.needsPoints(config::ETHERDREAM_MIN_PACKET_POINTS)) {
                requestPoints(req);
                sendPoints();
            }

            if (beginRequired) {
                sendBegin();
            }
        }

        close();
        if (running) {
            std::this_thread::sleep_for(retryDelay);
        }
    }
}

expected<Ack>
EtherDreamController::waitForResponse(char command) {
    if (!running) {
        return unexpected(std::make_error_code(std::errc::operation_canceled));
    }
    if (!tcpClient.is_connected()) {
        return unexpected(make_error_code(std::errc::not_connected));
    }

    std::array<std::uint8_t, 22> raw{};

    while (true) {
        std::size_t bytesTransferred = 0;
        if (auto ec = tcpClient.read_exact(raw.data(), raw.size(), &bytesTransferred); ec) {
            if (ec == asio::error::timed_out) {
                logError("[EtherDream] RX timeout waiting for command", command);
                recordIntermittentError(error_types::network::timeout);
            }
            logError("[EtherDream] RX error", ec.value(), ec.category().name(), ec.message());
            recordConnectionError(error_types::network::receiveFailed);
            return unexpected(std::error_code(ec.value(), ec.category()));
        }

        EtherDreamResponse response;
        if (!response.decode(raw.data(), raw.size())) {
            logError("[EtherDreamController] Failed to decode ACK for command", command);
            recordIntermittentError(error_types::network::protocolError);
            return unexpected(make_error_code(std::errc::protocol_error));
        }

        // The DAC can reply with 'I' when it has dropped back to idle (e.g., frame ended)
        // and the command we sent is no longer valid. Treat this as an idle/NACK and
        // request a new prepare/begin rather than waiting for an ACK that will never come.
        if (response.response == 'I' && static_cast<char>(response.command) == command) {
            updatePlaybackRequirements(response.status, /*commandAcked*/ false);
            logError("[EtherDream] received 'I' (invalid/idle) for command", command,
                     "sts", response.status.describe());
            recordIntermittentError(error_types::network::protocolError);
            return unexpected(make_error_code(std::errc::operation_canceled));
        }

        const bool ackMatched = (response.response == 'a') &&
                                (static_cast<char>(response.command) == command);

        updatePlaybackRequirements(response.status, ackMatched);

        logInfoVerbose("[EtherDream] RX", static_cast<char>(response.response),
                       "cmd", command, "sts", response.status.describe());

        if (response.response == 'I') {
            continue; // status frame only; wait for actual ACK
        }

        if (!ackMatched) {
            logError("[EtherDream] unexpected ACK expected", command,
                     "got response", static_cast<char>(response.response),
                     "for command", static_cast<char>(response.command),
                     "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()));
            recordIntermittentError(error_types::network::protocolError);
            return unexpected(make_error_code(std::errc::protocol_error));
        }

        return Ack{response.status, static_cast<char>(response.command)};
    }
}



void EtherDreamController::close() {
    logInfoVerbose("[EtherDreamController] close()");
    connectionActive = false;
    setConnectionState(false);
    // Keep the operation idempotent so repeated calls are harmless.
    if (!tcpClient.is_open()) {
        return;
    }
    // Future improvement: cancel outstanding async operations before closing.
    tcpClient.close();
}

bool EtherDreamController::isConnected() const {
    return tcpClient.is_connected();
}

bool EtherDreamController::hasActiveConnection() const {
    return tcpClient.is_connected() && !lastNetworkError().has_value() && connectionActive;
}

void EtherDreamController::setPointRate(std::uint32_t pointRateValue) {
    LaserControllerStreaming::setPointRate(pointRateValue);
    {
        std::lock_guard<std::mutex> lock(pendingRatesMutex);
        pendingRateChanges.push_back(pointRateValue);
    }
}

std::optional<std::uint16_t> EtherDreamController::nextPendingRateChange() {
    std::lock_guard<std::mutex> lock(pendingRatesMutex);
    if (pendingRateChanges.empty()) {
        return std::nullopt;
    }
    auto next = pendingRateChanges.back();
    pendingRateChanges.clear();
    return next;
}



expected<Ack> EtherDreamController::sendCommand() {

    if (!running) {
        return unexpected(std::make_error_code(std::errc::operation_canceled));
    }

    if (!commandBuffer.isReady()) {
        return unexpected(make_error_code(std::errc::invalid_argument));
    }

    const char opcode = commandBuffer.commandOpcode();
    const auto sendStartTime = std::chrono::steady_clock::now();

    if (auto ec = tcpClient.write_all(commandBuffer.data(), commandBuffer.size()); ec) {
        commandBuffer.reset();
        recordConnectionError(error_types::network::sendFailed);
        return unexpected(ec);
    }

    auto ack = waitForResponse(opcode);
    if (ack && opcode == 'd') {
        recordLatencySample(std::chrono::steady_clock::now() - sendStartTime);
    }
    commandBuffer.reset();
    return ack;
}

expected<Ack> EtherDreamController::sendPointRate(std::uint16_t rate) {

    commandBuffer.setPointRateCommand(static_cast<std::uint32_t>(rate));

    logError("[EtherDream] TX 'q'", "rate", rate,
             "timeout_ms", tcpClient.defaultTimeout().count());

    auto ack = sendCommand();
    if (!ack) {
        if (ack.error() == asio::error::timed_out) {
        logError("[EtherDream] point-rate command timed out after",
                 tcpClient.defaultTimeout().count(), "ms");
        recordIntermittentError(error_types::network::timeout);
        }
        return ack;
    }

    pendingRateChangeCount++;

    return ack;
}

std::size_t EtherDreamController::calculateMinimumPoints() {


    const int bufferFullness = estimateBufferFullness();

    const int minPoints = core::BufferEstimator::minimumBufferPoints(
        getPointRate(),
        config::ETHERDREAM_MIN_BUFFER_MS,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS));
    if(bufferFullness>=minPoints) return 0; 
    else return static_cast<std::size_t>(minPoints - bufferFullness);
    
}


void EtherDreamController::handleNetworkFailure(std::string_view where,
                                     const std::error_code& ec) {
    logError("[EtherDreamController] failure", where, ec.message());
    connectionActive = false;
    lastError = ec;
    recordConnectionError(error_types::network::connectionLost);
}


void EtherDreamController::updatePlaybackRequirements(const EtherDreamStatus& status, bool commandAcked) {
    const bool wasUnderflow = (lastKnownStatus.playbackFlags & 0x04u) != 0;
    lastKnownStatus = status;
    lastReceiveTime = std::chrono::steady_clock::now();
    lastEstimatedBufferFullness.store(status.bufferFullness, std::memory_order_relaxed);
    lastKnownBufferCapacity.store(getBufferSize(), std::memory_order_relaxed);
    

    const bool estop = status.lightEngineState == LightEngineState::Estop;
    const bool underflow = (status.playbackFlags & 0x04u) != 0;
    if (underflow && !wasUnderflow) {
        recordIntermittentError(error_types::network::bufferUnderflow);
    }
    clearRequired = estop || underflow || !commandAcked;

    prepareRequired = !clearRequired
        && status.lightEngineState == LightEngineState::Ready
        && status.playbackState == PlaybackState::Idle;

    const std::size_t bufferFullness = static_cast<std::size_t>(status.bufferFullness);
    beginRequired = !clearRequired
        && status.playbackState == PlaybackState::Prepared
        && bufferFullness >= config::ETHERDREAM_MIN_PACKET_POINTS;
}

core::PointFillRequest EtherDreamController::getFillRequest() {
    // Build a "how many points do we need right now?" request using the latest
    // projected controller-buffer fullness.
    const auto bufferFullness = estimateBufferFullness();

    const auto bufferCapacity = getBufferSize();
    const auto freeSpace = bufferCapacity > bufferFullness ? bufferCapacity - bufferFullness : 0;
    const auto minimumPointsRequired =
    std::min<std::size_t>(calculateMinimumPoints(), freeSpace);
    
    core::PointFillRequest req;
    req.maximumPointsRequired = freeSpace;
    req.minimumPointsRequired = minimumPointsRequired;
    // Convert queue depth (points) into time so scheduled frame callbacks can
    // place new content close to the moment it will actually appear.
    const auto bufferLead = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::milli>(pointsToMillis(bufferFullness)));

    req.estimatedFirstPointRenderTime =
        std::chrono::steady_clock::now() + bufferLead; 

    logInfoVerbose("[EtherDreamController :: getFillRequest]", "buffer :", bufferFullness, " | min :", req.minimumPointsRequired, " | max :", req.maximumPointsRequired); 

    pointsToSend.clear();
    return req;
}

void EtherDreamController::sendPoints() {
    if (clearRequired || prepareRequired) {
        resetPoints();
        return;
    }

    if (pointsToSend.size() == 0) {
        return;
    }

    const bool injectRateChange = pendingRateChangeCount>0;
    const std::uint16_t pointCount =  static_cast<std::uint16_t>(pointsToSend.size());

    commandBuffer.setDataCommand(pointCount);

    for (std::size_t idx = 0; idx < pointCount; ++idx) {
        const bool setRateBit = injectRateChange && idx == 0;
        commandBuffer.addPoint(pointsToSend[idx], setRateBit);
    }

    if (commandBuffer.size() == 0) {
        handleNetworkFailure("packet serialization", std::make_error_code(std::errc::invalid_argument));
        resetPoints();
        return;
    }

    auto dataAck = sendCommand();
    if (!dataAck) {
        if (dataAck.error() != std::errc::operation_canceled) {
            handleNetworkFailure("data command", dataAck.error());
        }
        resetPoints();
        return;
    }

    if (injectRateChange) {
        pendingRateChangeCount--;
    }

    resetPoints();
}

void EtherDreamController::sendClear() {
    logInfoVerbose("[EtherDream] clear required -> send 'c'");
    commandBuffer.setSingleByteCommand('c');
    if (auto ack = sendCommand(); !ack) {
        if (ack.error() != std::errc::operation_canceled) {
            handleNetworkFailure("clear command", ack.error());
        }
    }
}

void EtherDreamController::sendPrepare() {
    logError("[EtherDream] prepare required -> send 'p'");
    commandBuffer.setSingleByteCommand('p');
    if (auto ack = sendCommand(); !ack) {
        if (ack.error() != std::errc::operation_canceled) {
            handleNetworkFailure("prepare command", ack.error());
        }
    }
}

void EtherDreamController::sendBegin() {
    logError("[EtherDream] begin required -> send 'b'");
    const auto targetRate = getPointRate();
    commandBuffer.setBeginCommand(targetRate);
        if (auto ack = sendCommand(); !ack) {
            if (ack.error() == asio::error::timed_out) {
                logError("[EtherDream] begin write timeout after",
                         tcpClient.defaultTimeout().count(), "ms");
                recordIntermittentError(error_types::network::timeout);
            }
            if (ack.error() != asio::error::timed_out &&
                ack.error() != std::errc::operation_canceled) {
                handleNetworkFailure("begin command", ack.error());
            }
        }
}

expected<Ack> EtherDreamController::sendPing() {
    commandBuffer.setSingleByteCommand('?');
    return sendCommand();
}

int EtherDreamController::estimateBufferFullness() const {
    bool projected = false;
    const int estimated = calculateBufferFullnessFromAnchor(
        lastKnownStatus.bufferFullness,
        lastReceiveTime,
        lastKnownStatus.pointRate,
        lastKnownStatus.bufferFullness,
        &projected);
    if (!projected) {
        lastEstimatedBufferFullness.store(lastKnownStatus.bufferFullness, std::memory_order_relaxed);
        return lastKnownStatus.bufferFullness;
    }

    const int bufferSize = getBufferSize();
    const int clamped = clampBufferFullnessToCapacity(estimated, bufferSize);
    lastEstimatedBufferFullness.store(clamped, std::memory_order_relaxed);
    lastKnownBufferCapacity.store(bufferSize, std::memory_order_relaxed);
    return clamped;
}

void EtherDreamController::sleepUntilNextPoints() {
    // Strategy:
    // 1) Decide the minimum safe buffer level based on point rate and a
    //    short target buffer duration.
    // 2) Estimate how long until the current buffer drains to that level.
    // 3) Sleep for that time, clamped to a small min/max window.

    int minPointsInBuffer = core::BufferEstimator::minimumBufferPoints(
        getPointRate(),
        config::ETHERDREAM_MIN_BUFFER_MS,
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS));
    const int bufferSize = getBufferSize();
    const int minPacketPoints = static_cast<int>(config::ETHERDREAM_MIN_PACKET_POINTS);

    if ((bufferSize - minPointsInBuffer) < minPacketPoints) {
        minPointsInBuffer = bufferSize - minPacketPoints;
    }

    // Estimate how long until the buffer drains to that minimum.
    const int fullness = static_cast<int>(estimateBufferFullness());
    const int pointsAboveMinimum = std::max(0, fullness - minPointsInBuffer);

    int millisToWait = static_cast<int>(std::llround(
        pointsToMillis(static_cast<std::size_t>(pointsAboveMinimum))));

    const int minSleep = static_cast<int>(config::ETHERDREAM_MIN_SLEEP.count());
    const int maxSleep = static_cast<int>(config::ETHERDREAM_MAX_SLEEP.count());

    millisToWait = core::BufferEstimator::clampSleepMillis(
        millisToWait,
        minSleep,
        maxSleep);

    logInfoVerbose("[EtherDreamController] Sleeping for", millisToWait, "ms");
    std::this_thread::sleep_for(std::chrono::milliseconds{millisToWait});
}

void EtherDreamController::resetPoints() {
    pointsToSend.clear();
}


int EtherDreamController::getBufferSize() const {
    if (controllerInfo) {
        const int size = controllerInfo->bufferSizeValue();
        if (size > 0) {
            return size;
        }
    }
    return 0;
}

std::optional<core::BufferState> EtherDreamController::getBufferState() const {
    return buildBufferState(
        lastKnownBufferCapacity.load(std::memory_order_relaxed),
        lastEstimatedBufferFullness.load(std::memory_order_relaxed));
}

bool EtherDreamController::ensureConnected() {
    if (tcpClient.is_connected()) {
        setConnectionState(true);
        return true;
    }

    if (tcpClient.is_open()) {
        tcpClient.close();
    }

    auto result = connect();
    if (!result) {
        lastError = result.error();
        setConnectionState(false);
        return false;
    }

    lastError.reset();
    setConnectionState(true);
    return true;
}

bool EtherDreamController::performHandshake() {
    if (auto initialAck = waitForResponse('?'); initialAck) {
        return true;
    }

    auto pingAck = sendPing();
    if (pingAck) {
        return true;
    }

    handleNetworkFailure("initial ping", pingAck.error());
    return false;
}

std::optional<std::error_code> EtherDreamController::lastNetworkError() const {
    return lastError;
}

void EtherDreamController::clearNetworkError() {
    lastError.reset();
}

} // namespace libera::etherdream
