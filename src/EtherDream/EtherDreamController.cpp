/**
 * @brief Implements the EtherDream DAC worker loop: connection, polling, and streaming.
 */
#include "libera/etherdream/EtherDreamController.hpp"

#include "libera/core/BufferEstimator.hpp"
#include "libera/core/ByteRead.hpp"
#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
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

namespace {

bool isKnownLightEngineState(LightEngineState state) {
    switch (state) {
        case LightEngineState::Ready:
        case LightEngineState::Warmup:
        case LightEngineState::Cooldown:
        case LightEngineState::Estop:
            return true;
    }
    return false;
}

bool isKnownPlaybackState(PlaybackState state) {
    switch (state) {
        case PlaybackState::Idle:
        case PlaybackState::Prepared:
        case PlaybackState::Playing:
        case PlaybackState::Paused:
            return true;
    }
    return false;
}

std::uint16_t readLe16OrZero(const std::uint8_t* data,
                             std::size_t size,
                             std::size_t offset) {
    if (!data || size < offset + sizeof(std::uint16_t)) {
        return 0;
    }
    return core::bytes::readLe16(data + offset);
}

std::uint32_t readLe32OrZero(const std::uint8_t* data,
                             std::size_t size,
                             std::size_t offset) {
    if (!data || size < offset + sizeof(std::uint32_t)) {
        return 0;
    }
    return core::bytes::readLe32(data + offset);
}

std::size_t loggedCommandBytes(char opcode, std::size_t commandSize) {
    if (opcode == 'd') {
        // A full data command can be thousands of bytes; the header plus the
        // first point is enough to verify count and rate-change control bit.
        return std::min<std::size_t>(commandSize, 21);
    }
    return commandSize;
}

bool isLifecycleCommand(char opcode) {
    switch (opcode) {
        case 'b':
        case 'q':
        case 'p':
        case 's':
        case 'c':
            return true;
        default:
            return false;
    }
}

bool isPlaybackStateThatCanHoldBufferedData(PlaybackState state) {
    return state == PlaybackState::Prepared || state == PlaybackState::Playing;
}

bool statusLooksFullForDataReject(const EtherDreamStatus& status, int bufferSize) {
    if (bufferSize <= 0) {
        return status.bufferFullness > 0;
    }
    const auto reportedFullness = static_cast<int>(status.bufferFullness);
    // Real Ether Dreams have returned I d instead of F d for oversized packets.
    // In those cases the status reports the advertised capacity or one less.
    return reportedFullness >= std::max(0, bufferSize - 1);
}

double durationMillis(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double roundedMillis(double value) {
    return std::round(value * 10.0) / 10.0;
}

std::string trimFirmwareString(std::string value) {
    const auto isTrimSpace = [](unsigned char c) {
        return std::isspace(c) != 0;
    };

    auto first = std::find_if_not(value.begin(), value.end(), [&](char c) {
        return isTrimSpace(static_cast<unsigned char>(c));
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [&](char c) {
        return isTrimSpace(static_cast<unsigned char>(c));
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

} // namespace

EtherDreamController::EtherDreamController() = default;

EtherDreamController::EtherDreamController(EtherDreamControllerInfo info)
: controllerInfo(std::move(info)) {
    if (controllerInfo) {
        firmwareVersionString = controllerInfo->firmwareVersion();
    }
}

EtherDreamController::~EtherDreamController() {
    // Orderly shutdown: stop the worker thread and close the TCP connection.
    stopThread();
    close();
}

expected<void> EtherDreamController::connect(const EtherDreamControllerInfo& info) {
    controllerInfo = info;
    {
        std::scoped_lock<std::mutex> lock(firmwareMutex);
        firmwareVersionString = info.firmwareVersion();
    }
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

    // Set timeouts before connecting so they apply to this and all future attempts.
    tcpClient.setDefaultTimeout(200ms);
    tcpClient.setConnectTimeout(1s);

    if (auto connectError = tcpClient.connect(endpoint); connectError) {
        logError("[EtherDreamController] connect failed", connectError.message(),
                 "target", controllerInfo->ip(), controllerInfo->port(),
                 "timeout_ms", tcpClient.getConnectTimeout().count());
        lastError = connectError;
        recordConnectionError(error_types::network::connectFailed);
        return unexpected(connectError);
    }
    // Ask the socket to send small packets right away and keep the connection alive.
    tcpClient.setLowLatency();


    ++connectionGeneration;
    resetProtocolStateForConnection();
    logInfo("[EtherDream] connected",
            "conn", connectionGeneration,
            "target", controllerInfo->ip(), controllerInfo->port());
    clearNetworkError();
    setConnectionState(true);

    return {};
}

void EtherDreamController::run() {
    auto retryDelay = config::ETHERDREAM_RECONNECT_INITIAL_DELAY;
    const auto resetRetryDelay = [&] {
        retryDelay = config::ETHERDREAM_RECONNECT_INITIAL_DELAY;
    };
    const auto sleepBeforeRetry = [&](bool increaseDelay) {
        if (retryDelay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(retryDelay);
        }
        if (increaseDelay) {
            if (retryDelay == std::chrono::milliseconds::zero()) {
                retryDelay = config::ETHERDREAM_RECONNECT_BACKOFF_DELAY;
            } else {
                retryDelay = std::min(retryDelay * 2,
                                      config::ETHERDREAM_RECONNECT_MAX_DELAY);
            }
        }
    };

    while (running) {
        if (!controllerInfo) {
            setConnectionState(false);
            std::this_thread::sleep_for(config::ETHERDREAM_RECONNECT_BACKOFF_DELAY);
            continue;
        }

        if (!ensureConnected()) {
            sleepBeforeRetry(true);
            continue;
        }

        connectionActive = true;
        setConnectionState(true);

        if (!performHandshake()) {
            close();
            sleepBeforeRetry(true);
            continue;
        }
        const auto connectionStartedAt = std::chrono::steady_clock::now();
        // make sure to blank points if reconnecting
        resetStartupBlank();

        while (running && connectionActive) {
            if (stopRequired) {
                sendStop();
                continue;
            }

            if (clearRequired) {
                sendClear();
                continue;
            }

            if (prepareRequired) {
                sendPrepare();
                continue;
            }

            if (beginRequired) {
                sendBegin();
                continue;
            }

            syncPointRate();
            if (stopRequired || clearRequired || prepareRequired || beginRequired) {
                continue;
            }

            if (!canSendData()) {
                pollStatus();
                std::this_thread::sleep_for(config::ETHERDREAM_MIN_SLEEP);
                continue;
            }

            sleepUntilNextPoints();

            auto req = getFillRequest();
            if (shouldRequestPoints(req)) {
                const auto requestStart = std::chrono::steady_clock::now();
                const bool requestRan = requestPoints(req);
                const auto requestDuration =
                    std::chrono::steady_clock::now() - requestStart;
                captureStreamHealthRequest(req, requestRan, requestDuration);
                if (requestRan) {
                    sendPoints();
                } else {
                    recordStreamHealthRequestMiss();
                    resetPoints();
                }
            }
        }

        sendOrderlyStopBeforeClose();
        close();
        if (running) {
            const bool connectionWasStable =
                std::chrono::steady_clock::now() - connectionStartedAt >=
                config::ETHERDREAM_RECONNECT_STABLE_RESET_TIME;
            if (connectionWasStable) {
                resetRetryDelay();
            }
            sleepBeforeRetry(!connectionWasStable);
        }
    }
}

expected<Ack>
EtherDreamController::waitForResponse(char command,
                                      bool allowWhileStopping,
                                      std::uint64_t sequence) {
    if (!running && !allowWhileStopping) {
        return unexpected(std::make_error_code(std::errc::operation_canceled));
    }
    if (!tcpClient.is_connected()) {
        return unexpected(make_error_code(std::errc::not_connected));
    }

    std::array<std::uint8_t, 22> raw{};

    while (true) {
        std::size_t bytesTransferred = 0;
        if (auto ec = tcpClient.read_exact(raw.data(), raw.size(), &bytesTransferred); ec) {
            const auto partialBytes = std::min<std::size_t>(bytesTransferred, raw.size());
            if (ec == asio::error::timed_out) {
                logError("[EtherDream] RX timeout waiting for command", command,
                         "conn", connectionGeneration,
                         "seq", sequence,
                         "bytes", bytesTransferred,
                         "partial_hex", EtherDreamStatus::toHexLine(raw.data(), partialBytes),
                         "tx", describeProtocolTx(sequence));
                recordIntermittentError(error_types::network::timeout);
            } else {
                logError("[EtherDream] RX error", ec.value(), ec.category().name(), ec.message(),
                         "conn", connectionGeneration,
                         "seq", sequence,
                         "cmd", command,
                         "bytes", bytesTransferred,
                         "partial_hex", EtherDreamStatus::toHexLine(raw.data(), partialBytes),
                         "tx", describeProtocolTx(sequence));
            }
            recordConnectionError(error_types::network::receiveFailed);
            return unexpected(std::error_code(ec.value(), ec.category()));
        }

        EtherDreamResponse response;
        if (!response.decode(raw.data(), raw.size())) {
            logError("[EtherDreamController] Failed to decode ACK for command", command,
                     "conn", connectionGeneration,
                     "seq", sequence,
                     "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                     "tx", describeProtocolTx(sequence));
            recordIntermittentError(error_types::network::protocolError);
            return unexpected(make_error_code(std::errc::protocol_error));
        }

        logProtocolRx(sequence, command, response, raw.data(), raw.size());

        if (response.response == '!' && static_cast<char>(response.command) == command) {
            updatePlaybackRequirements(response.status);
            logError("[EtherDream] stop condition active for command", command,
                     "conn", connectionGeneration,
                     "seq", sequence,
                     "sts", response.status.describe(),
                     "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                     "tx", describeProtocolTx(sequence));
            recordIntermittentError(error_types::etherdream::stopCondition);
            return unexpected(make_error_code(std::errc::operation_canceled));
        }

        // 'I' is a NAK for an invalid command. The accompanying status tells us
        // which recovery command should come next.
        if (response.response == 'I' && static_cast<char>(response.command) == command) {
            updatePlaybackRequirements(response.status);
            const bool dataRejectedBecausePlaybackIdle =
                command == 'd'
                && response.status.lightEngineState == LightEngineState::Ready
                && response.status.playbackState == PlaybackState::Idle;
            const bool dataRejectedBecauseBufferFull =
                command == 'd'
                && response.status.lightEngineState == LightEngineState::Ready
                && isPlaybackStateThatCanHoldBufferedData(response.status.playbackState)
                && (statusLooksFullForDataReject(response.status, getBufferSize())
                    || dataPacketWouldOverflowBuffer(response.status, sequence));
            if (dataRejectedBecausePlaybackIdle) {
                if (response.status.hasPlaybackUnderflow()) {
                    logInfo("[EtherDream] data command rejected after playback underflow",
                            "conn", connectionGeneration,
                            "seq", sequence,
                            "sts", response.status.describe(),
                            "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                            "tx", describeProtocolTx(sequence));
                    recordIntermittentError(error_types::network::bufferUnderflow);
                } else {
                    logInfo("[EtherDream] data command rejected because playback is idle after underrun",
                            "conn", connectionGeneration,
                            "seq", sequence,
                            "sts", response.status.describe(),
                            "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                            "tx", describeProtocolTx(sequence));
                    recordIntermittentError(error_types::network::bufferUnderflow);
                }
                resetStartupBlank();
                return unexpected(make_error_code(std::errc::operation_canceled));
            }
            if (dataRejectedBecauseBufferFull) {
                logError("[EtherDream] data command rejected because DAC buffer is full",
                         "conn", connectionGeneration,
                         "seq", sequence,
                         "sts", response.status.describe(),
                         "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                         "tx", describeProtocolTx(sequence));
                recordBufferOverrun();
                return unexpected(make_error_code(std::errc::operation_canceled));
            }
            logError("[EtherDream] NAK invalid command", command,
                     "conn", connectionGeneration,
                     "seq", sequence,
                     "sts", response.status.describe(),
                     "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                     "tx", describeProtocolTx(sequence));
            recordIntermittentError(error_types::network::protocolError);
            return unexpected(make_error_code(std::errc::operation_canceled));
        }

        if (response.response == 'F' && static_cast<char>(response.command) == command) {
            updatePlaybackRequirements(response.status);
            logError("[EtherDream] NAK buffer full for command", command,
                     "conn", connectionGeneration,
                     "seq", sequence,
                     "sts", response.status.describe(),
                     "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                     "tx", describeProtocolTx(sequence));
            recordBufferOverrun();
            return unexpected(make_error_code(std::errc::operation_canceled));
        }

        const bool ackMatched = (response.response == 'a') &&
                                (static_cast<char>(response.command) == command);

        updatePlaybackRequirements(response.status);

        if (!ackMatched) {
            logError("[EtherDream] unexpected ACK expected", command,
                     "conn", connectionGeneration,
                     "seq", sequence,
                     "got response", static_cast<char>(response.response),
                     "for command", static_cast<char>(response.command),
                     "hex", EtherDreamStatus::toHexLine(raw.data(), raw.size()),
                     "tx", describeProtocolTx(sequence));
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

std::string EtherDreamController::firmwareVersion() const {
    std::scoped_lock<std::mutex> lock(firmwareMutex);
    return firmwareVersionString;
}

std::string EtherDreamController::hardwareVersion() const {
    if (!controllerInfo) {
        return {};
    }
    return controllerInfo->hardwareVersion();
}

void EtherDreamController::setPointRate(std::uint32_t pointRateValue) {
    const std::uint32_t safeRate = maxSafePointRate();
    const std::uint32_t clampedRate =
        safeRate > 0 ? std::min(pointRateValue, safeRate) : pointRateValue;
    if (clampedRate != pointRateValue) {
        logError("[EtherDream] requested point rate exceeds safe maximum",
                 "requested", pointRateValue,
                 "clamped", clampedRate);
    }

    core::LaserControllerStreaming::setPointRate(clampedRate);
}

void EtherDreamController::syncPointRate() {
    const auto desired = getPointRate();
    if (desired == lastSentPointRate) {
        return;
    }
    if (lastSentPointRate == 0) {
        return;
    }
    if (lastKnownStatus.playbackState != PlaybackState::Playing) {
        return;
    }

    logInfo("[EtherDream] point rate changed while playing; restarting stream",
            "from", lastSentPointRate,
            "to", desired,
            "sts", lastKnownStatus.describe());
    stopRequired = true;
    clearRequired = false;
    prepareRequired = false;
    beginRequired = false;
    pendingRateChangeCount = 0;
    resetPoints();
}

expected<Ack> EtherDreamController::sendCommand(bool allowWhileStopping) {

    if (!running && !allowWhileStopping) {
        return unexpected(std::make_error_code(std::errc::operation_canceled));
    }

    if (!commandBuffer.isReady()) {
        return unexpected(make_error_code(std::errc::invalid_argument));
    }

    const char opcode = commandBuffer.commandOpcode();
    const auto sequence = ++nextCommandSequence;
    const auto sendStartTime = std::chrono::steady_clock::now();

    recordProtocolTx(sequence, opcode);

    if (auto ec = tcpClient.write_all(commandBuffer.data(), commandBuffer.size()); ec) {
        logError("[EtherDream] TX write failed",
                 "conn", connectionGeneration,
                 "seq", sequence,
                 "cmd", opcode,
                 "bytes", commandBuffer.size(),
                 "error", ec.value(), ec.category().name(), ec.message(),
                 "tx", describeProtocolTx(sequence));
        commandBuffer.reset();
        recordConnectionError(error_types::network::sendFailed);
        return unexpected(ec);
    }

    // Once bytes are on the wire we must drain the matching response even if
    // shutdown starts. Otherwise the next orderly stop can read this command's
    // ACK and falsely report a protocol error.
    auto ack = waitForResponse(opcode, /*allowWhileStopping*/ true, sequence);
    if (ack && opcode == 'd') {
        recordLatencySample(std::chrono::steady_clock::now() - sendStartTime);
    }
    commandBuffer.reset();
    return ack;
}

expected<Ack> EtherDreamController::sendPointRate(std::uint32_t rate) {

    commandBuffer.setPointRateCommand(rate);

    auto ack = sendCommand();
    if (!ack) {
        if (ack.error() == asio::error::timed_out) {
            logError("[EtherDream] point-rate command timed out after",
                     tcpClient.getDefaultTimeout().count(), "ms");
            recordIntermittentError(error_types::network::timeout);
        }
        return ack;
    }

    pendingRateChangeCount++;

    return ack;
}

void EtherDreamController::recordProtocolTx(std::uint64_t sequence, char opcode) {
    const auto* data = commandBuffer.data();
    const auto size = commandBuffer.size();

    auto& snapshot = protocolTxHistory[nextProtocolTxHistoryIndex];
    nextProtocolTxHistoryIndex = (nextProtocolTxHistoryIndex + 1) % protocolTxHistory.size();

    snapshot = ProtocolTxSnapshot{};
    snapshot.valid = true;
    snapshot.timestamp = std::chrono::steady_clock::now();
    snapshot.sequence = sequence;
    snapshot.opcode = opcode;
    snapshot.bytes = size;
    snapshot.pendingRateChangeCount = pendingRateChangeCount;
    snapshot.localRate = getPointRate();
    snapshot.lastSentRate = lastSentPointRate;
    snapshot.status = lastKnownStatus;
    snapshot.hex = EtherDreamStatus::toHexLine(data, loggedCommandBytes(opcode, size));

    if (opcode == 'd') {
        snapshot.pointCount = readLe16OrZero(data, size, 1);
        snapshot.firstControl = readLe16OrZero(data, size, 3);
        snapshot.rateChangeBit = (snapshot.firstControl & 0x8000u) != 0;
        logInfoVerbose("[EtherDream] TX",
                       "conn", connectionGeneration,
                       "seq", sequence,
                       "cmd", opcode,
                       "bytes", size,
                       "points", snapshot.pointCount,
                       "first_control", snapshot.firstControl,
                       "rate_change_bit", snapshot.rateChangeBit ? 1 : 0,
                       "pending_rate_changes", snapshot.pendingRateChangeCount,
                       "local_rate", snapshot.localRate,
                       "last_sent_rate", snapshot.lastSentRate,
                       "status", snapshot.status.describe(),
                       "hex", snapshot.hex);
        return;
    }

    if (opcode == 'b') {
        snapshot.beginFlags = readLe16OrZero(data, size, 1);
        snapshot.commandRate = readLe32OrZero(data, size, 3);
        logInfo("[EtherDream] TX",
                "conn", connectionGeneration,
                "seq", sequence,
                "cmd", opcode,
                "bytes", size,
                "begin_flags", snapshot.beginFlags,
                "rate", snapshot.commandRate,
                "local_rate", snapshot.localRate,
                "last_sent_rate", snapshot.lastSentRate,
                "status", snapshot.status.describe(),
                "hex", snapshot.hex);
        return;
    }

    if (opcode == 'q') {
        snapshot.commandRate = readLe32OrZero(data, size, 1);
        logInfo("[EtherDream] TX",
                "conn", connectionGeneration,
                "seq", sequence,
                "cmd", opcode,
                "bytes", size,
                "rate", snapshot.commandRate,
                "local_rate", snapshot.localRate,
                "last_sent_rate", snapshot.lastSentRate,
                "pending_rate_changes", snapshot.pendingRateChangeCount,
                "status", snapshot.status.describe(),
                "hex", snapshot.hex);
        return;
    }

    if (isLifecycleCommand(opcode)) {
        logInfo("[EtherDream] TX",
                "conn", connectionGeneration,
                "seq", sequence,
                "cmd", opcode,
                "bytes", size,
                "local_rate", snapshot.localRate,
                "last_sent_rate", snapshot.lastSentRate,
                "status", snapshot.status.describe(),
                "hex", snapshot.hex);
    } else {
        logInfoVerbose("[EtherDream] TX",
                       "conn", connectionGeneration,
                       "seq", sequence,
                       "cmd", opcode,
                       "bytes", size,
                       "local_rate", snapshot.localRate,
                       "last_sent_rate", snapshot.lastSentRate,
                       "status", snapshot.status.describe(),
                       "hex", snapshot.hex);
    }
}

void EtherDreamController::logProtocolRx(std::uint64_t sequence,
                                         char expectedCommand,
                                         const EtherDreamResponse& response,
                                         const std::uint8_t* raw,
                                         std::size_t rawSize) const {
    const bool ackMatched = response.response == 'a'
        && static_cast<char>(response.command) == expectedCommand;
    if (!ackMatched) {
        return;
    }
    if (!isLifecycleCommand(expectedCommand) && !isVerbose()) {
        return;
    }

    logInfo("[EtherDream] RX",
            "conn", connectionGeneration,
            "seq", sequence,
            "expected", expectedCommand,
            "response", static_cast<char>(response.response),
            "cmd", static_cast<char>(response.command),
            "ack_matched", ackMatched ? 1 : 0,
            "sts", response.status.describe(),
            "hex", EtherDreamStatus::toHexLine(raw, rawSize));
}

const EtherDreamController::ProtocolTxSnapshot*
EtherDreamController::findProtocolTxSnapshot(std::uint64_t sequence) const {
    for (const auto& snapshot : protocolTxHistory) {
        if (snapshot.valid && snapshot.sequence == sequence) {
            return &snapshot;
        }
    }
    return nullptr;
}

std::string EtherDreamController::describeProtocolTx(std::uint64_t sequence) const {
    const auto* snapshot = findProtocolTxSnapshot(sequence);
    if (!snapshot) {
        return "unavailable";
    }

    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - snapshot->timestamp);

    std::ostringstream os;
    os << "{seq=" << snapshot->sequence
       << " cmd=" << snapshot->opcode
       << " bytes=" << snapshot->bytes
       << " age_ms=" << age.count()
       << " local_rate=" << snapshot->localRate
       << " last_sent_rate=" << snapshot->lastSentRate;

    if (snapshot->opcode == 'd') {
        os << " points=" << snapshot->pointCount
           << " first_control=" << snapshot->firstControl
           << " rate_change_bit=" << (snapshot->rateChangeBit ? 1 : 0)
           << " pending_rate_changes=" << snapshot->pendingRateChangeCount;
    } else if (snapshot->opcode == 'b') {
        os << " begin_flags=" << snapshot->beginFlags
           << " rate=" << snapshot->commandRate;
    } else if (snapshot->opcode == 'q') {
        os << " rate=" << snapshot->commandRate
           << " pending_rate_changes=" << snapshot->pendingRateChangeCount;
    }

    os << " status_at_tx={" << snapshot->status.describe()
       << "} hex=" << snapshot->hex
       << "}";
    return os.str();
}

std::size_t EtherDreamController::calculateMinimumPoints() {
    const int bufferFullness = estimateBufferFullness();
    const int minPoints = targetBufferPoints();
    if(bufferFullness>=minPoints) return 0;
    else return static_cast<std::size_t>(minPoints - bufferFullness);

}


void EtherDreamController::handleNetworkFailure(std::string_view where,
                                     const std::error_code& ec) {
    logError("[EtherDreamController] failure", where, ec.message());
    connectionActive = false;
    lastError = ec;
    recordConnectionError(error_types::network::connectionLost);
    // Once a command/ACK exchange fails, the TCP stream may be out of sync.
    // Close immediately so the worker cannot send another command on stale
    // socket state before the outer loop starts a clean connection.
    tcpClient.close();
}


void EtherDreamController::updatePlaybackRequirements(const EtherDreamStatus& status) {
    const bool wasUnderflow = lastKnownStatus.hasPlaybackUnderflow();
    lastKnownStatus = status;
    lastReceiveTime = std::chrono::steady_clock::now();
    lastEstimatedBufferFullness.store(status.bufferFullness, std::memory_order_relaxed);
    lastKnownBufferCapacity.store(getBufferSize(), std::memory_order_relaxed);
    

    const bool lightEngineEstop = status.lightEngineState == LightEngineState::Estop;
    const bool underflow = status.hasPlaybackUnderflow();
    if (underflow && !wasUnderflow) {
        recordIntermittentError(error_types::network::bufferUnderflow);
        resetStartupBlank();
    }

    const bool knownLightEngineState = isKnownLightEngineState(status.lightEngineState);
    const bool knownPlaybackState = isKnownPlaybackState(status.playbackState);

    stopRequired = false;
    clearRequired = false;
    prepareRequired = false;
    beginRequired = false;

    if (!knownLightEngineState) {
        return;
    }

    if (lightEngineEstop) {
        clearRequired = true;
        return;
    }

    if (status.lightEngineState != LightEngineState::Ready) {
        return;
    }

    if (!knownPlaybackState || status.playbackState == PlaybackState::Paused) {
        stopRequired = true;
        return;
    }

    if (statusPointRateIsImplausible(status)) {
        logError("[EtherDream] reported implausible active point rate; resetting playback",
                 "reported", status.pointRate,
                 "max", maxSafePointRate(),
                 "sts", status.describe());
        recordIntermittentError(error_types::network::protocolError);
        stopRequired = true;
        resetPoints();
        return;
    }

    // Command validity is governed by the reported playback state. Flags such
    // as underflow are useful diagnostics, but may be sticky; do not send
    // prepare while the DAC still says it is prepared/playing.
    prepareRequired = status.playbackState == PlaybackState::Idle;

    const std::size_t bufferFullness = static_cast<std::size_t>(status.bufferFullness);
    beginRequired = status.playbackState == PlaybackState::Prepared
        && bufferFullness >= config::ETHERDREAM_MIN_PACKET_POINTS;
}

void EtherDreamController::applyFreshConnectionStatus(const EtherDreamStatus& status) {
    if (status.lightEngineState == LightEngineState::Estop) {
        stopRequired = false;
        clearRequired = true;
        prepareRequired = false;
        beginRequired = false;
        return;
    }

    if (status.lightEngineState != LightEngineState::Ready) {
        stopRequired = false;
        clearRequired = false;
        prepareRequired = false;
        beginRequired = false;
        return;
    }

    if (status.playbackState == PlaybackState::Idle) {
        stopRequired = false;
        clearRequired = false;
        prepareRequired = true;
        beginRequired = false;
        return;
    }

    // Match the legacy ofxLaser Ether Dream path: a DAC that greets us already
    // prepared/playing may be carrying stale state from a previous connection,
    // so stop it before preparing our own stream.
    logInfoVerbose("[EtherDream] fresh connection reported non-idle playback; resetting",
                   "sts", status.describe());
    stopRequired = true;
    clearRequired = false;
    prepareRequired = false;
    beginRequired = false;
    resetPoints();
}

core::PointFillRequest EtherDreamController::getFillRequest() {
    // Build a "how many points do we need right now?" request using the latest
    // projected controller-buffer fullness.
    const auto bufferFullness = estimateBufferFullness();
    const int targetPoints = targetBufferPoints();

    const auto freeSpace = usableBufferFreeSpace(bufferFullness);
    const auto packetSpace = std::min<std::size_t>(freeSpace, config::ETHERDREAM_MAX_PACKET_POINTS);
    const auto targetDeficit =
        std::min<std::size_t>(calculateMinimumPoints(), packetSpace);
    
    core::PointFillRequest req;
    // The latency target is the amount we require, not a hard cap on the
    // current frame. If a frame is partly emitted and the Ether Dream has real
    // FIFO space, allow the scheduler to send more of it in this packet.
    req.maximumPointsRequired = packetSpace;
    req.minimumPointsRequired = targetDeficit;
    // Convert queue depth (points) into time so scheduled frame callbacks can
    // place new content close to the moment it will actually appear.
    const auto bufferLead = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::milli>(pointsToMillis(bufferFullness)));

    req.estimatedFirstPointRenderTime =
        std::chrono::steady_clock::now() + bufferLead; 

    logInfoVerbose("[EtherDreamController :: getFillRequest]", "buffer :", bufferFullness, " | min :", req.minimumPointsRequired, " | max :", req.maximumPointsRequired); 

    pendingStreamHealthRequest.estimatedBufferBeforeRequest = bufferFullness;
    pendingStreamHealthRequest.targetBufferPointCount = targetPoints;
    pendingStreamHealthRequest.freeSpace = freeSpace;
    pendingStreamHealthRequest.playbackWasPlaying =
        lastKnownStatus.playbackState == PlaybackState::Playing;
    pendingStreamHealthRequest.computerPerformanceUnderrun = false;
    pendingStreamHealthRequest.recoveryBlankApplied = false;
    pointsToSend.clear();
    return req;
}

bool EtherDreamController::shouldRequestPoints(const core::PointFillRequest& request) const {
    if (lastKnownStatus.lightEngineState == LightEngineState::Ready
        && lastKnownStatus.playbackState == PlaybackState::Prepared
        && lastKnownStatus.bufferFullness < config::ETHERDREAM_MIN_PACKET_POINTS) {
        return request.minimumPointsRequired > 0;
    }

    if (lastKnownStatus.lightEngineState == LightEngineState::Ready
        && lastKnownStatus.playbackState == PlaybackState::Playing
        && bufferIsBelowPlayingUnderrunThreshold(
            pendingStreamHealthRequest.estimatedBufferBeforeRequest)) {
        return request.maximumPointsRequired > 0;
    }

    return request.minimumPointsRequired >= config::ETHERDREAM_MIN_PACKET_POINTS;
}

bool EtherDreamController::canSendData() const {
    if (stopRequired || clearRequired || prepareRequired || beginRequired) {
        return false;
    }

    if (lastKnownStatus.lightEngineState != LightEngineState::Ready) {
        return false;
    }

    return lastKnownStatus.playbackState == PlaybackState::Prepared
        || lastKnownStatus.playbackState == PlaybackState::Playing;
}

void EtherDreamController::sendPoints() {
    if (!canSendData()) {
        if (!clearRequired
            && !stopRequired
            && lastKnownStatus.lightEngineState == LightEngineState::Ready
            && lastKnownStatus.playbackState == PlaybackState::Idle) {
            prepareRequired = true;
        }
        resetPoints();
        return;
    }

    if (pointsToSend.size() == 0) {
        return;
    }

    if (pendingStreamHealthRequestLikelyStarvedDac()) {
        recordComputerPerformanceUnderrun();
        applyUnderrunRecoveryBlankToCurrentPacket();
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

    const auto sendStart = std::chrono::steady_clock::now();
    auto dataAck = sendCommand();
    const auto sendDuration = std::chrono::steady_clock::now() - sendStart;
    if (!dataAck) {
        if (dataAck.error() != std::errc::operation_canceled) {
            handleNetworkFailure("data command", dataAck.error());
        }
        resetPoints();
        return;
    }

    recordStreamHealthPacket(dataAck->status, sendDuration);

    if (injectRateChange) {
        pendingRateChangeCount--;
    }

    resetPoints();
}

void EtherDreamController::sendStop(bool allowWhileStopping) {
    logInfoVerbose("[EtherDream] stop required -> send 's'");
    commandBuffer.setSingleByteCommand('s');
    if (auto ack = sendCommand(allowWhileStopping); ack) {
        lastSentPointRate = 0;
        pendingRateChangeCount = 0;
        resetPoints();
        if (ack->status.lightEngineState == LightEngineState::Ready
            && ack->status.playbackState != PlaybackState::Idle) {
            logError("[EtherDream] stop ACK did not report idle playback",
                     "sts", ack->status.describe());
            stopRequired = true;
            clearRequired = false;
            prepareRequired = false;
            beginRequired = false;
        }
    } else {
        if (ack.error() != std::errc::operation_canceled) {
            handleNetworkFailure("stop command", ack.error());
        }
    }
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
    logInfoVerbose("[EtherDream] prepare required -> send 'p'");
    commandBuffer.setSingleByteCommand('p');
    if (auto ack = sendCommand(); ack) {
        if (ack->status.lightEngineState == LightEngineState::Ready
            && ack->status.playbackState != PlaybackState::Idle
            && ack->status.playbackState != PlaybackState::Prepared) {
            logError("[EtherDream] prepare ACK did not report idle/prepared playback",
                     "sts", ack->status.describe());
            stopRequired = true;
            clearRequired = false;
            prepareRequired = false;
            beginRequired = false;
        }
    } else {
        if (ack.error() != std::errc::operation_canceled) {
            handleNetworkFailure("prepare command", ack.error());
        }
    }
}

void EtherDreamController::sendBegin() {
    logInfoVerbose("[EtherDream] begin required -> send 'b'");
    const auto targetRate = getPointRate();
    commandBuffer.setBeginCommand(targetRate);
    if (auto ack = sendCommand(); ack) {
        if (ack->status.playbackState == PlaybackState::Playing) {
            lastSentPointRate = targetRate;
            pendingRateChangeCount = 0;
        }
    } else {
        if (ack.error() == asio::error::timed_out) {
            logError("[EtherDream] begin write timeout after",
                     tcpClient.getDefaultTimeout().count(), "ms");
            recordIntermittentError(error_types::network::timeout);
        }
        if (ack.error() != std::errc::operation_canceled) {
            handleNetworkFailure("begin command", ack.error());
        }
    }
}

expected<Ack> EtherDreamController::sendPing() {
    commandBuffer.setSingleByteCommand('?');
    return sendCommand();
}

void EtherDreamController::pollStatus() {
    auto ack = sendPing();
    if (ack && statusReportsPlayingBufferUnderrun(ack->status)) {
        recordIntermittentError(error_types::network::bufferUnderflow);
        resetStartupBlank();
    }
    if (!ack && ack.error() != std::errc::operation_canceled) {
        handleNetworkFailure("status ping", ack.error());
    }
}

void EtherDreamController::sendOrderlyStopBeforeClose() {
    if (!connectionActive || !tcpClient.is_connected()) {
        return;
    }

    if (lastKnownStatus.lightEngineState != LightEngineState::Ready) {
        return;
    }

    if (lastKnownStatus.playbackState == PlaybackState::Idle) {
        return;
    }

    // Legacy ofxLaser sent a non-emergency stop before closing. Do the same
    // from the worker thread so shutdown cannot race with an in-flight command.
    sendStop(/*allowWhileStopping*/ true);
}

void EtherDreamController::StreamHealthSamples::reset() {
    count = 0;
    next = 0;
}

void EtherDreamController::StreamHealthSamples::add(double value) {
    values[next] = value;
    next = (next + 1) % STREAM_HEALTH_SAMPLE_CAPACITY;
    count = std::min<std::size_t>(count + 1, STREAM_HEALTH_SAMPLE_CAPACITY);
}

double EtherDreamController::StreamHealthSamples::percentile(double fraction) const {
    if (count == 0) {
        return 0.0;
    }

    std::array<double, STREAM_HEALTH_SAMPLE_CAPACITY> sorted = values;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count));

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const double rawIndex = clamped * static_cast<double>(count - 1);
    const auto lower = static_cast<std::size_t>(std::floor(rawIndex));
    const auto upper = static_cast<std::size_t>(std::ceil(rawIndex));
    if (lower == upper) {
        return sorted[lower];
    }

    const double weight = rawIndex - static_cast<double>(lower);
    return sorted[lower] + ((sorted[upper] - sorted[lower]) * weight);
}

double EtherDreamController::StreamHealthSamples::max() const {
    if (count == 0) {
        return 0.0;
    }
    return *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
}

void EtherDreamController::captureStreamHealthRequest(
    const core::PointFillRequest& request,
    bool requestRan,
    std::chrono::steady_clock::duration requestDuration) {

    pendingStreamHealthRequest.valid = true;
    pendingStreamHealthRequest.requestRan = requestRan;
    pendingStreamHealthRequest.request = request;
    pendingStreamHealthRequest.metrics = lastPointRequestMetrics();
    pendingStreamHealthRequest.requestDuration = requestDuration;
    pendingStreamHealthRequest.requestCompletedAt = std::chrono::steady_clock::now();
}

bool EtherDreamController::pendingStreamHealthRequestLikelyStarvedDac() const {
    if (!pendingStreamHealthRequest.valid ||
        !pendingStreamHealthRequest.playbackWasPlaying ||
        pendingStreamHealthRequest.targetBufferPointCount <= 0) {
        return false;
    }

    if (bufferIsBelowPlayingUnderrunThreshold(
            pendingStreamHealthRequest.estimatedBufferBeforeRequest)) {
        return true;
    }

    const int threshold = static_cast<int>(playingUnderrunBufferThreshold());
    const int pointsUntilUnderrun =
        pendingStreamHealthRequest.estimatedBufferBeforeRequest - threshold + 1;
    if (pointsUntilUnderrun <= 0) {
        return true;
    }

    const double msUntilUnderrun =
        pointsToMillis(static_cast<std::size_t>(pointsUntilUnderrun));
    return msUntilUnderrun > 0.0 &&
           durationMillis(pendingStreamHealthRequest.requestDuration) >= msUntilUnderrun;
}

void EtherDreamController::recordStreamHealthRequestMiss() {
    if (!pendingStreamHealthRequest.valid) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (streamHealthWindow.startedAt == std::chrono::steady_clock::time_point{}) {
        streamHealthWindow.startedAt = now;
    }

    streamHealthWindow.requestMisses++;
    streamHealthWindow.requestSamplesMs.add(
        durationMillis(pendingStreamHealthRequest.requestDuration));
    streamHealthWindow.lastTargetBufferPoints =
        pendingStreamHealthRequest.targetBufferPointCount;
    streamHealthWindow.minEstimatedBuffer = std::min(
        streamHealthWindow.minEstimatedBuffer,
        pendingStreamHealthRequest.estimatedBufferBeforeRequest);

    const bool likelyStarved = pendingStreamHealthRequestLikelyStarvedDac();
    if (likelyStarved) {
        streamHealthWindow.starvationEvents++;
        recordComputerPerformanceUnderrun();
        resetStartupBlank();
    }

    const int lowBufferThreshold = std::max<int>(
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        pendingStreamHealthRequest.targetBufferPointCount / 2);
    if (pendingStreamHealthRequest.estimatedBufferBeforeRequest <= lowBufferThreshold) {
        streamHealthWindow.lowBufferEvents++;
        const bool shouldLogWarning =
            isVerbose()
            && (lastStreamHealthWarningTime == std::chrono::steady_clock::time_point{}
                || now - lastStreamHealthWarningTime >= 500ms);
        if (shouldLogWarning) {
            lastStreamHealthWarningTime = now;
            logInfo("[EtherDream starvation risk] point request did not produce data",
                    "conn", connectionGeneration,
                    "rate", getPointRate(),
                    "est_buffer", pendingStreamHealthRequest.estimatedBufferBeforeRequest,
                    "buffer_ms", roundedMillis(pointsToMillis(
                        static_cast<std::size_t>(std::max(
                            pendingStreamHealthRequest.estimatedBufferBeforeRequest, 0)))),
                    "target", pendingStreamHealthRequest.targetBufferPointCount,
                    "free_space", pendingStreamHealthRequest.freeSpace,
                    "req_min", pendingStreamHealthRequest.request.minimumPointsRequired,
                    "req_max", pendingStreamHealthRequest.request.maximumPointsRequired,
                    "likely_starved", likelyStarved ? 1 : 0,
                    "request_ms", roundedMillis(durationMillis(
                        pendingStreamHealthRequest.requestDuration)));
        }
    }

    pendingStreamHealthRequest.valid = false;
    maybeLogStreamHealthSummary(now);
}

void EtherDreamController::recordStreamHealthPacket(
    const EtherDreamStatus& ackStatus,
    std::chrono::steady_clock::duration sendDuration) {

    if (!pendingStreamHealthRequest.valid) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (streamHealthWindow.startedAt == std::chrono::steady_clock::time_point{}) {
        streamHealthWindow.startedAt = now;
    }

    double dataGapMs = 0.0;
    if (lastStreamHealthDataAckTime != std::chrono::steady_clock::time_point{}) {
        dataGapMs = durationMillis(now - lastStreamHealthDataAckTime);
        streamHealthWindow.dataGapSamplesMs.add(dataGapMs);
    }
    lastStreamHealthDataAckTime = now;

    const auto& pending = pendingStreamHealthRequest;
    const auto& metrics = pending.metrics;
    streamHealthWindow.packetCount++;
    streamHealthWindow.requestSamplesMs.add(durationMillis(pending.requestDuration));
    streamHealthWindow.sendSamplesMs.add(durationMillis(sendDuration));
    streamHealthWindow.lastTargetBufferPoints = pending.targetBufferPointCount;
    streamHealthWindow.minEstimatedBuffer = std::min(
        streamHealthWindow.minEstimatedBuffer,
        pending.estimatedBufferBeforeRequest);
    streamHealthWindow.minAckBuffer = std::min(
        streamHealthWindow.minAckBuffer,
        static_cast<int>(ackStatus.bufferFullness));

    if (metrics.blankPaddingPointCount > 0) {
        streamHealthWindow.paddedEvents++;
        streamHealthWindow.paddedPoints += metrics.blankPaddingPointCount;
    }
    if (metrics.clampedPointCount > 0) {
        streamHealthWindow.clampedEvents++;
        streamHealthWindow.clampedPoints += metrics.clampedPointCount;
    }

    const bool computerPerformanceUnderrun =
        pending.computerPerformanceUnderrun || pendingStreamHealthRequestLikelyStarvedDac();
    const bool dacStoppedAfterPlaying =
        pending.playbackWasPlaying &&
        ackStatus.lightEngineState == LightEngineState::Ready &&
        ackStatus.playbackState == PlaybackState::Idle;
    const bool dacBufferUnderrunWhilePlaying =
        pending.playbackWasPlaying && statusReportsPlayingBufferUnderrun(ackStatus);
    const bool actualDacUnderrun = dacStoppedAfterPlaying || dacBufferUnderrunWhilePlaying;

    if (computerPerformanceUnderrun) {
        streamHealthWindow.starvationEvents++;
        if (!pending.computerPerformanceUnderrun) {
            recordComputerPerformanceUnderrun();
        }
        if (!pending.recoveryBlankApplied) {
            resetStartupBlank();
        }
    } else if (actualDacUnderrun) {
        streamHealthWindow.starvationEvents++;
        recordIntermittentError(error_types::network::bufferUnderflow);
        resetStartupBlank();
    }

    const int lowBufferThreshold = std::max<int>(
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        pending.targetBufferPointCount / 2);
    const bool lowBuffer =
        pending.estimatedBufferBeforeRequest <= lowBufferThreshold
        || static_cast<int>(ackStatus.bufferFullness) <= lowBufferThreshold;
    if (lowBuffer) {
        streamHealthWindow.lowBufferEvents++;
        const bool shouldLogWarning =
            isVerbose()
            && (lastStreamHealthWarningTime == std::chrono::steady_clock::time_point{}
                || now - lastStreamHealthWarningTime >= 500ms);
        if (shouldLogWarning) {
            lastStreamHealthWarningTime = now;
            logInfo("[EtherDream starvation risk]",
                    "conn", connectionGeneration,
                    "rate", getPointRate(),
                    "est_buffer", pending.estimatedBufferBeforeRequest,
                    "ack_buffer", ackStatus.bufferFullness,
                    "buffer_ms", roundedMillis(pointsToMillis(
                        static_cast<std::size_t>(std::max(
                            pending.estimatedBufferBeforeRequest, 0)))),
                    "ack_buffer_ms", roundedMillis(pointsToMillis(ackStatus.bufferFullness)),
                    "target", pending.targetBufferPointCount,
                    "free_space", pending.freeSpace,
                    "req_min", pending.request.minimumPointsRequired,
                    "req_max", pending.request.maximumPointsRequired,
                    "generated", metrics.generatedPointCount,
                    "final", metrics.finalPointCount,
                    "padded", metrics.blankPaddingPointCount,
                    "clamped", metrics.clampedPointCount,
                    "computer_underrun", computerPerformanceUnderrun ? 1 : 0,
                    "dac_underrun", actualDacUnderrun ? 1 : 0,
                    "request_ms", roundedMillis(durationMillis(pending.requestDuration)),
                    "send_ms", roundedMillis(durationMillis(sendDuration)),
                    "gap_ms", roundedMillis(dataGapMs),
                    "sts", ackStatus.describe());
        }
    }

    pendingStreamHealthRequest.valid = false;
    maybeLogStreamHealthSummary(now);
}

void EtherDreamController::maybeLogStreamHealthSummary(
    std::chrono::steady_clock::time_point now) {

    if (streamHealthWindow.startedAt == std::chrono::steady_clock::time_point{}) {
        streamHealthWindow.startedAt = now;
        return;
    }

    const auto elapsed = now - streamHealthWindow.startedAt;
    if (elapsed < 1s) {
        return;
    }

    const bool hasActivity =
        streamHealthWindow.packetCount > 0
        || streamHealthWindow.requestMisses > 0
        || streamHealthWindow.lowBufferEvents > 0;
    if (hasActivity && isVerbose()) {
        const int minEstimated =
            streamHealthWindow.minEstimatedBuffer == std::numeric_limits<int>::max()
                ? 0
                : streamHealthWindow.minEstimatedBuffer;
        const int minAck =
            streamHealthWindow.minAckBuffer == std::numeric_limits<int>::max()
                ? 0
                : streamHealthWindow.minAckBuffer;

        logInfo("[EtherDream health]",
                "conn", connectionGeneration,
                "rate", getPointRate(),
                "target", streamHealthWindow.lastTargetBufferPoints,
                "capacity", getBufferSize(),
                "packets", streamHealthWindow.packetCount,
                "request_misses", streamHealthWindow.requestMisses,
                "min_est", minEstimated,
                "min_ack", minAck,
                "gap_p95_ms", roundedMillis(streamHealthWindow.dataGapSamplesMs.percentile(0.95)),
                "gap_max_ms", roundedMillis(streamHealthWindow.dataGapSamplesMs.max()),
                "request_p95_ms", roundedMillis(streamHealthWindow.requestSamplesMs.percentile(0.95)),
                "request_max_ms", roundedMillis(streamHealthWindow.requestSamplesMs.max()),
                "send_p95_ms", roundedMillis(streamHealthWindow.sendSamplesMs.percentile(0.95)),
                "send_max_ms", roundedMillis(streamHealthWindow.sendSamplesMs.max()),
                "padded_events", streamHealthWindow.paddedEvents,
                "padded_points", streamHealthWindow.paddedPoints,
                "clamped_events", streamHealthWindow.clampedEvents,
                "clamped_points", streamHealthWindow.clampedPoints,
                "starvation_events", streamHealthWindow.starvationEvents,
                "low_buffer_events", streamHealthWindow.lowBufferEvents);
    }

    streamHealthWindow = StreamHealthWindow{};
    streamHealthWindow.startedAt = now;
}

void EtherDreamController::resetStreamHealth() {
    pendingStreamHealthRequest = PendingStreamHealthRequest{};
    streamHealthWindow = StreamHealthWindow{};
    streamHealthWindow.startedAt = std::chrono::steady_clock::now();
    lastStreamHealthDataAckTime = {};
    lastStreamHealthWarningTime = {};
}

void EtherDreamController::recordComputerPerformanceUnderrun() {
    if (!pendingStreamHealthRequest.computerPerformanceUnderrun) {
        pendingStreamHealthRequest.computerPerformanceUnderrun = true;
        recordIntermittentError(error_types::etherdream::streamStarvation);
    }
}

void EtherDreamController::recordBufferOverrun() {
    recordIntermittentError(error_types::network::bufferOverrun);
    // A full-buffer rejection means the DAC did not accept the packet we just
    // pulled from the scheduler. The next accepted packet may therefore jump
    // forward in content; blank its leading points just like an underrun restart.
    resetStartupBlank();
}

void EtherDreamController::applyUnderrunRecoveryBlankToCurrentPacket() {
    if (pendingStreamHealthRequest.recoveryBlankApplied) {
        return;
    }

    resetStartupBlank();
    applyStartupBlankToOutputPoints(pointsToSend);
    pendingStreamHealthRequest.recoveryBlankApplied = true;
}

int EtherDreamController::estimateBufferFullness() const {
    bool projected = false;
    const std::uint32_t projectionRate =
        lastKnownStatus.playbackState == PlaybackState::Playing ? getPointRate() : 0;
    const int estimated = calculateBufferFullnessFromSnapshot(
        lastKnownStatus.bufferFullness,
        lastReceiveTime,
        projectionRate,
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

int EtherDreamController::targetBufferPoints() const {
    return core::BufferEstimator::targetBufferPoints(
        getPointRate(),
        getBufferSize(),
        targetLatency(),
        static_cast<int>(config::ETHERDREAM_MIN_BUFFER_POINTS),
        static_cast<int>(config::ETHERDREAM_SAFETY_HEADROOM_POINTS));
}

int EtherDreamController::usableBufferFreeSpace(int bufferFullness) const {
    const int bufferSize = getBufferSize();
    if (bufferSize <= 0) {
        return 0;
    }

    const int clampedFullness = clampBufferFullnessToCapacity(bufferFullness, bufferSize);
    const int rawFreeSpace = std::max(0, bufferSize - clampedFullness);
    const int writeHeadroom = std::min<int>(
        rawFreeSpace,
        static_cast<int>(config::ETHERDREAM_WRITE_HEADROOM_POINTS));
    return rawFreeSpace - writeHeadroom;
}

bool EtherDreamController::dataPacketWouldOverflowBuffer(
    const EtherDreamStatus& status,
    std::uint64_t sequence) const {
    const int bufferSize = getBufferSize();
    if (bufferSize <= 0) {
        return false;
    }

    const auto* snapshot = findProtocolTxSnapshot(sequence);
    if (!snapshot || snapshot->opcode != 'd' || snapshot->pointCount == 0) {
        return false;
    }

    return static_cast<int>(snapshot->pointCount) >
        usableBufferFreeSpace(status.bufferFullness);
}

std::uint32_t EtherDreamController::maxSafePointRate() const {
    std::uint32_t maxRate = config::ETHERDREAM_MAX_REASONABLE_POINT_RATE;
    if (controllerInfo && controllerInfo->maxPointRate() > 0) {
        maxRate = std::min(maxRate, controllerInfo->maxPointRate());
    }
    return maxRate;
}

bool EtherDreamController::usesDmaBufferUnderrunThreshold() const {
    if (!controllerInfo) {
        return false;
    }

    // ED3 reports hardware revision 30, ED4 reports 40/41. Unknown/virtual
    // devices keep the older "only zero is definitely empty" threshold.
    return controllerInfo->hardwareRevision() >= 30;
}

std::size_t EtherDreamController::playingUnderrunBufferThreshold() const {
    return usesDmaBufferUnderrunThreshold()
        ? config::ETHERDREAM_MIN_BUFFER_POINTS
        : std::size_t{1};
}

bool EtherDreamController::bufferIsBelowPlayingUnderrunThreshold(int bufferFullness) const {
    return bufferFullness < static_cast<int>(playingUnderrunBufferThreshold());
}

bool EtherDreamController::statusReportsPlayingBufferUnderrun(
    const EtherDreamStatus& status) const {
    return status.lightEngineState == LightEngineState::Ready
        && status.playbackState == PlaybackState::Playing
        && bufferIsBelowPlayingUnderrunThreshold(status.bufferFullness);
}

bool EtherDreamController::statusPointRateIsImplausible(const EtherDreamStatus& status) const {
    if (status.playbackState != PlaybackState::Prepared
        && status.playbackState != PlaybackState::Playing) {
        return false;
    }
    return status.pointRate > maxSafePointRate();
}

void EtherDreamController::sleepUntilNextPoints() {
    // Strategy:
    // 1) Decide the minimum safe buffer level based on point rate and a
    //    short target buffer duration.
    // 2) Estimate how long until the current buffer drains to that level.
    // 3) Sleep for that time, clamped to a small min/max window.

    int minPointsInBuffer = targetBufferPoints();

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

void EtherDreamController::resetProtocolStateForConnection() {
    clearRequired = false;
    stopRequired = false;
    prepareRequired = true;
    beginRequired = false;
    connectionActive = false;
    lastKnownStatus = EtherDreamStatus{};
    lastReceiveTime = {};
    pendingRateChangeCount = 0;
    lastSentPointRate = 0;
    nextCommandSequence = 0;
    protocolTxHistory = {};
    nextProtocolTxHistoryIndex = 0;
    resetStreamHealth();
    lastEstimatedBufferFullness.store(0, std::memory_order_relaxed);
    lastKnownBufferCapacity.store(getBufferSize(), std::memory_order_relaxed);
    resetPoints();
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

bool EtherDreamController::shouldProbeFirmwareVersion() const {
    if (firmwareProbeDisabledForSession || !controllerInfo) {
        return false;
    }

    if (controllerInfo->hardwareRevision() == 0 || controllerInfo->softwareRevision() < 2) {
        return false;
    }

    return firmwareVersion().empty();
}

expected<std::string> EtherDreamController::readFirmwareVersionString() {
    constexpr char firmwareCommand = 'v';
    if (auto ec = tcpClient.write_all(&firmwareCommand, 1); ec) {
        logInfo("[EtherDream] firmware version query write failed; continuing without firmware version",
                "conn", connectionGeneration,
                "error", ec.value(), ec.category().name(), ec.message());
        return unexpected(ec);
    }

    std::array<std::uint8_t, 32> raw{};
    std::size_t bytesTransferred = 0;
    if (auto ec = tcpClient.read_exact(raw.data(), raw.size(), &bytesTransferred); ec) {
        const auto partialBytes = std::min<std::size_t>(bytesTransferred, raw.size());
        logInfo("[EtherDream] firmware version query read failed; continuing without firmware version",
                "conn", connectionGeneration,
                "bytes", bytesTransferred,
                "partial_hex", EtherDreamStatus::toHexLine(raw.data(), partialBytes),
                "error", ec.value(), ec.category().name(), ec.message());
        return unexpected(ec);
    }

    std::string text;
    text.reserve(raw.size());
    for (std::uint8_t byte : raw) {
        const unsigned char c = static_cast<unsigned char>(byte);
        text.push_back((byte == 0 || std::isprint(c) == 0) ? ' ' : static_cast<char>(byte));
    }
    return trimFirmwareString(std::move(text));
}

bool EtherDreamController::probeFirmwareVersionOnExistingConnection() {
    if (!shouldProbeFirmwareVersion()) {
        return true;
    }

    // This undocumented command comes from Jacob Potter's original sitter
    // utility. Run it only on the already-owned TCP connection, before any
    // stream commands, so we never create a second Ether Dream connection.
    auto version = readFirmwareVersionString();
    if (!version) {
        firmwareProbeDisabledForSession = true;
        return true;
    }

    if (!version->empty()) {
        {
            std::scoped_lock<std::mutex> lock(firmwareMutex);
            firmwareVersionString = *version;
        }
        if (controllerInfo) {
            controllerInfo->setFirmwareVersion(*version);
        }
        logInfo("[EtherDream] firmware version",
                "conn", connectionGeneration,
                "version", *version);
    }

    return true;
}

bool EtherDreamController::performHandshake() {
    if (auto initialAck = waitForResponse('?'); initialAck) {
        applyFreshConnectionStatus(initialAck->status);
        return probeFirmwareVersionOnExistingConnection();
    }

    auto pingAck = sendPing();
    if (pingAck) {
        applyFreshConnectionStatus(pingAck->status);
        return probeFirmwareVersionOnExistingConnection();
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
