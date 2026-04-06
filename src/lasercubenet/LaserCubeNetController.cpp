#include "libera/lasercubenet/LaserCubeNetController.hpp"

#include "libera/core/ByteRead.hpp"
#include "libera/core/ByteBuffer.hpp"
#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <thread>

namespace libera::lasercubenet {

namespace error_types = libera::core::error_types;

constexpr auto ackDisconnectThreshold = std::chrono::milliseconds(500);
constexpr auto reconnectRetryDelay = std::chrono::milliseconds(100);

LaserCubeNetController::LaserCubeNetController() {
    // Reuse the shared IO context so sockets share the same network thread.
    io = net::shared_io_context();
}

LaserCubeNetController::LaserCubeNetController(LaserCubeNetControllerInfo info)
    : LaserCubeNetController() {
    ipAddress = info.ipAddress();
}

LaserCubeNetController::~LaserCubeNetController() {
    stopThread();
    close();
}

void LaserCubeNetController::updateDiscoveredStatus(const LaserCubeNetStatus& status) {
    {
        std::lock_guard<std::mutex> lock(latestStatusMutex);
        latestStatus = status;
    }

    maxPointRate.store(status.pointRateMax, std::memory_order_relaxed);
    pointBufferCapacity.store(status.bufferMax, std::memory_order_relaxed);

    const auto clampedRate =
        LaserCubeNetConfig::clampPointRate(getPointRate(), status.pointRateMax);
    LaserControllerStreaming::setPointRate(clampedRate);

    if (!networkConnected.load(std::memory_order_relaxed)) {
        reconnectRequested.store(true, std::memory_order_relaxed);
    }
}

libera::expected<void> LaserCubeNetController::connect(const LaserCubeNetControllerInfo& info) {
    updateDiscoveredStatus(info.status());
    return connectToStatus(info.status());
}

libera::expected<void> LaserCubeNetController::connectToStatus(const LaserCubeNetStatus& status) {
    ipAddress = status.ipAddress;
    maxPointRate.store(status.pointRateMax, std::memory_order_relaxed);
    pointBufferCapacity.store(status.bufferMax, std::memory_order_relaxed);
    const auto initialRate =
        LaserCubeNetConfig::clampPointRate(getPointRate(), status.pointRateMax);
    LaserControllerStreaming::setPointRate(initialRate);
    messageTimes.fill(std::chrono::steady_clock::time_point{});
    pendingAckCount = 0;

    lastAckTime = std::chrono::steady_clock::now();
    lastAckWarningTime = std::chrono::steady_clock::time_point{};
    lastUnexpectedAckSenderLogTime = std::chrono::steady_clock::time_point{};
    lastDataSentTime = std::chrono::steady_clock::time_point{};
    lastDataSentBufferSize = 0;
    lastReportedBufferFullness.store(0, std::memory_order_relaxed);
    lastEstimatedBufferFullness.store(0, std::memory_order_relaxed);

    if (!io) {
        io = net::shared_io_context();
    }

    if (dataSocket) {
        dataSocket->close();
    }
    if (commandSocket) {
        commandSocket->close();
    }

    // Data socket sends point packets; command socket handles control/status.
    dataSocket = std::make_unique<net::UdpSocket>(*io);
    commandSocket = std::make_unique<net::UdpSocket>(*io);

    if (auto ec = dataSocket->open_v4()) {
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(ec);
    }
    if (auto ec = dataSocket->bind_any(0)) {
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(ec);
    }
    if (auto ec = commandSocket->open_v4()) {
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(ec);
    }
    if (auto ec = commandSocket->bind_any(0)) {
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(ec);
    }

    std::error_code ecAddr;
    auto address = libera::net::asio::ip::make_address(ipAddress, ecAddr);
    if (ecAddr) {
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    dataEndpoint = libera::net::asio::ip::udp::endpoint(address, LaserCubeNetConfig::DATA_PORT);
    commandEndpoint = libera::net::asio::ip::udp::endpoint(address, LaserCubeNetConfig::COMMAND_PORT);

    networkConnected.store(true, std::memory_order_relaxed);
    reconnectRequested.store(false, std::memory_order_relaxed);
    setConnectionState(true);
    // Device's internal rate is unknown after a fresh connection, so force
    // the next syncPointRate() tick to push the current value.
    pointRatePushNeeded = true;
    resetStartupBlank();
    setVerbose(false);
    return {};
}

bool LaserCubeNetController::reconnectToLatestStatus() {
    std::optional<LaserCubeNetStatus> status;
    {
        std::lock_guard<std::mutex> lock(latestStatusMutex);
        status = latestStatus;
    }

    if (!status) {
        return false;
    }

    auto result = connectToStatus(*status);
    if (!result) {
        logError("[LaserCubeNetController] reconnect failed", result.error().message());
        return false;
    }

    return true;
}

void LaserCubeNetController::close() {
    networkConnected.store(false, std::memory_order_relaxed);
    reconnectRequested.store(false, std::memory_order_relaxed);
    setConnectionState(false);
    messageTimes.fill(std::chrono::steady_clock::time_point{});
    pendingAckCount = 0;
    if (dataSocket) {
        dataSocket->close();
    }
    if (commandSocket) {
        commandSocket->close();
    }
}

void LaserCubeNetController::run() {
    using namespace std::chrono_literals;

    resetStartupBlank();

    while (running.load()) {
        if (!networkConnected.load(std::memory_order_relaxed)) {
            setConnectionState(false);
            if (reconnectRequested.exchange(false, std::memory_order_relaxed)) {
                reconnectToLatestStatus();
            }
            std::this_thread::sleep_for(reconnectRetryDelay);
            continue;
        }

        setConnectionState(true);
        syncPointRate();

        // Send at most one packet per loop; pacing is handled by buffer estimation.
        (void)sendPoints();
        checkAcks();

        // Adaptive sleep: wait roughly as long as the buffer excess takes to drain.
        // This avoids a busy loop when the controller doesn't need more data yet.
        {
            const int bufferFullness = lastEstimatedBufferFullness.load(std::memory_order_relaxed);
            const auto activePointRate = lastSentPointRate;
            const int targetFull = LaserCubeNetConfig::targetBufferPoints(
                activePointRate, getTotalBufferCapacity(), targetLatency());
            const int excess = bufferFullness - targetFull;
            int msToWait = 1;
            if (excess > 0 && activePointRate > 0) {
                msToWait = static_cast<int>(std::llround(
                    pointsToMillis(static_cast<std::size_t>(excess), activePointRate)));
                msToWait = std::clamp(msToWait, 1, 10);
            }
            if (msToWait > 2) {
                logInfoVerbose("[LaserCubeNet] sleep", msToWait, "ms",
                               "excess", excess,
                               "buf", bufferFullness,
                               "target", targetFull);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(msToWait));
        }
    }
}

void LaserCubeNetController::setPointRate(std::uint32_t pointRateValue) {
    pointRateValue = LaserCubeNetConfig::clampPointRate(
        pointRateValue,
        maxPointRate.load(std::memory_order_relaxed));
    core::LaserControllerStreaming::setPointRate(pointRateValue);
}

bool LaserCubeNetController::sendPoints() {
    // Advance a notional frame index to mirror the LaserDockNet protocol.
    frameNumber++;

    const auto activePointRate = lastSentPointRate;

    // Estimate how full the controller buffer is right now.
    const int minEstimatedBufferFullness =
        std::max(
            calculateBufferFullnessFromSnapshot(
                lastDataSentBufferSize,
                lastDataSentTime,
                activePointRate,
                0),
            calculateBufferFullnessFromSnapshot(
                lastReportedBufferFullness.load(std::memory_order_relaxed),
                lastAckTime,
                activePointRate,
                0));
    lastEstimatedBufferFullness.store(minEstimatedBufferFullness, std::memory_order_relaxed);
    // Keep a latency-derived point cushion, but leave fixed packet headroom so
    // estimated fullness + delayed acks do not push the controller into overrun.
    const int targetBufferPoints =
        LaserCubeNetConfig::targetBufferPoints(
            activePointRate,
            getTotalBufferCapacity(),
            targetLatency());
    int maxPointsToAdd = std::max(
        0,
        targetBufferPoints - minEstimatedBufferFullness);

    const int maxPointsInPacket = static_cast<int>(LaserCubeNetConfig::MAX_POINTS_PER_PACKET);

    if (maxPointsToAdd <= 0) {
        return true;
    }

    // as we are only going to process one packet's worth of points every time, if we need more 
    // we'll just take what we can send right now, but then should ask again once it's sent.
    if (maxPointsToAdd > maxPointsInPacket) {
        maxPointsToAdd = maxPointsInPacket;
    }

    core::PointFillRequest request{};
    // LaserCubeNet sends one packet per loop, so require enough points to make
    // this packet worth transmitting when we've decided a refill is needed.
    request.minimumPointsRequired = static_cast<std::size_t>(maxPointsToAdd);
    request.maximumPointsRequired = static_cast<std::size_t>(maxPointsToAdd);
    const auto renderLead = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(
            pointsToMillis(static_cast<std::size_t>(minEstimatedBufferFullness), activePointRate)));
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now() + renderLead;

    logInfoVerbose("[LaserCubeNet] sendPoints",
                   "estBuf", minEstimatedBufferFullness,
                   "target", targetBufferPoints,
                   "toAdd", maxPointsToAdd,
                   "renderLeadMs", std::chrono::duration<double, std::milli>(renderLead).count());

    if (!requestPoints(request)) {
        return false;
    }

    if (pointsToSend.empty()) {
        return true;
    }

    // Safety clamp in case the callback overfilled.
    if (pointsToSend.size() > static_cast<std::size_t>(maxPointsToAdd)) {
        pointsToSend.resize(static_cast<std::size_t>(maxPointsToAdd));
    }

    // Build a single UDP packet: header + packed 12-bit points.
    //
    // Packet layout (byte offsets):
    //  0: CMD_SAMPLE_DATA (0xA9)
    //  1: Reserved, always 0x00
    //  2: messageNumber (uint8, wraps 0..255)
    //  3: frameNumber   (uint8, wraps 0..255)
    //  4..: Point data, each point is 10 bytes:
    //       x[0], x[1], y[0], y[1], r[0], r[1], g[0], g[1], b[0], b[1]
    //       All channels are unsigned 12-bit values stored little-endian in 16-bit slots.
    core::ByteBuffer packet;
    packet.appendUInt8(LaserCubeNetConfig::CMD_SAMPLE_DATA);
    packet.appendUInt8(0x00);
    packet.appendUInt8(messageNumber);
    packet.appendUInt8(frameNumber);

    for (const auto& pt : pointsToSend) {
        // LaserCube Net's scan orientation is mirrored on X relative to the
        // rest of libera backends, so flip X at controller output.
        packet.appendUInt16(encodeUnsigned12FromSignedUnit(-pt.x));
        packet.appendUInt16(encodeUnsigned12FromSignedUnit(pt.y));
        packet.appendUInt16(encodeUnsigned12FromUnit(pt.r));
        packet.appendUInt16(encodeUnsigned12FromUnit(pt.g));
        packet.appendUInt16(encodeUnsigned12FromUnit(pt.b));
    }

    const bool success = sendData(packet.data(), packet.size());
    if (success) {
        const auto now = std::chrono::steady_clock::now();
        if (messageTimes[messageNumber] == std::chrono::steady_clock::time_point{}) {
            ++pendingAckCount;
        }
        messageTimes[messageNumber] = now;
        lastDataSentTime = now;
        lastDataSentBufferSize = minEstimatedBufferFullness + static_cast<int>(pointsToSend.size());
    }

    messageNumber++;

    return success;
}

void LaserCubeNetController::syncPointRate() {
    const auto desired = getPointRate();
    // Short-circuit when nothing has changed and no resync is pending.
    if (!pointRatePushNeeded && desired == lastSentPointRate) {
        return;
    }
    core::ByteBuffer payload;
    payload.appendUInt32(desired);
    const bool ok = sendCommand(LaserCubeNetConfig::CMD_SET_ILDA_RATE, payload.data(), payload.size());
    if (ok) {
        lastSentPointRate = desired;
        pointRatePushNeeded = false;
    } else {
        // Leave the latch set so the next tick retries.
        pointRatePushNeeded = true;
    }
}

bool LaserCubeNetController::sendData(const std::uint8_t* buffer, std::size_t size) {
    if (!dataSocket || !buffer || size == 0) {
        recordConnectionError(error_types::network::sendFailed);
        networkConnected.store(false, std::memory_order_relaxed);
        return false;
    }
    auto ec = dataSocket->send_to(buffer, size, dataEndpoint, std::chrono::milliseconds(50));
    if (ec) {
        logError("[LaserCubeNetController] Failed to send data", ec.message());
        recordConnectionError(error_types::network::sendFailed);
        networkConnected.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool LaserCubeNetController::sendCommand(std::uint8_t cmd, const std::uint8_t* payload, std::size_t size) {
    if (!commandSocket) {
        recordConnectionError(error_types::network::sendFailed);
        networkConnected.store(false, std::memory_order_relaxed);
        return false;
    }

    core::ByteBuffer buffer;
    buffer.appendUInt8(cmd);
    for (std::size_t i = 0; i < size; ++i) {
        buffer.appendUInt8(payload ? payload[i] : 0);
    }

    auto ec = commandSocket->send_to(buffer.data(), buffer.size(), commandEndpoint, std::chrono::milliseconds(200));
    if (ec) {
        logError("[LaserCubeNetController] command send failed", ec.message());
        recordConnectionError(error_types::network::sendFailed);
        networkConnected.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void LaserCubeNetController::checkAcks() {
    if (!dataSocket) {
        return;
    }

    constexpr std::size_t maxAckPollsPerCall = 64;
    std::size_t polls = 0;
    const auto now = std::chrono::steady_clock::now();

    while (polls < maxAckPollsPerCall) {
        std::array<std::uint8_t, 4> buffer{};
        libera::net::asio::ip::udp::endpoint sender;
        std::size_t received = 0;
        auto ec = dataSocket->recv_from(buffer.data(), buffer.size(), sender,
                                        received, std::chrono::milliseconds(1), false);

        if (ec == libera::net::asio::error::timed_out ||
            ec == libera::net::asio::error::operation_aborted) {
            break;
        }
        if (ec) {
            logInfo("[LaserCubeNetController] ack receive failed", ec.message());
            recordIntermittentError(error_types::network::receiveFailed);
            break;
        }
        ++polls;

        if (sender.address() != dataEndpoint.address() || sender.port() != dataEndpoint.port()) {
            if (lastUnexpectedAckSenderLogTime == std::chrono::steady_clock::time_point{} ||
                (now - lastUnexpectedAckSenderLogTime) > std::chrono::seconds(1)) {
                logInfo("[LaserCubeNetController] ignoring ack from unexpected sender",
                        sender.address().to_string(), sender.port(),
                        "expected", dataEndpoint.address().to_string(), dataEndpoint.port());
                recordIntermittentError(error_types::network::packetLoss);
                lastUnexpectedAckSenderLogTime = now;
            }
            continue;
        }

        if (received != 4) {
            if (received > 0) {
                logInfo("[LaserCubeNetController] ack packet unexpected size", received);
                recordIntermittentError(error_types::network::protocolError);
            }
            continue;
        }

        if (buffer[0] != LaserCubeNetConfig::CMD_GET_RINGBUFFER_FREE) {
            logInfo("[LaserCubeNetController] DIFFERENT RESPONSE", static_cast<int>(buffer[0]));
            recordIntermittentError(error_types::network::protocolError);
            continue;
        }

        const std::uint8_t receivedMessageNumber = buffer[1];
        const auto& sentTime = messageTimes[receivedMessageNumber];
        if (sentTime == std::chrono::steady_clock::time_point{}) {
            recordIntermittentError(error_types::network::packetLoss);
            continue;
        }

        recordLatencySample(now - sentTime);
        lastAckTime = now;
        lastAckWarningTime = std::chrono::steady_clock::time_point{};

        const std::uint16_t bufferSpace = core::bytes::readLe16(&buffer[2]);
        const int bufferFullness = getTotalBufferCapacity() - static_cast<int>(bufferSpace);
        logInfoVerbose("[LaserCubeNet] ack",
                       "buf", bufferFullness,
                       "space", bufferSpace,
                       "rttMs", std::chrono::duration<double, std::milli>(now - sentTime).count());
        lastReportedBufferFullness.store(bufferFullness, std::memory_order_relaxed);
        lastEstimatedBufferFullness.store(bufferFullness, std::memory_order_relaxed);

        // Always reset the sent-data snapshot to ack ground truth.
        // The previous guard (sentTime >= lastDataSentTime) was never
        // satisfied because sendPoints() updates lastDataSentTime to now
        // on every packet, so the ack's sentTime was always older.  This
        // caused the sent-based buffer estimate to self-reinforce and
        // drift above the real hardware buffer level.
        lastDataSentTime = now;
        lastDataSentBufferSize = bufferFullness;

        if (bufferSpace == 0) {
            logInfo("[LaserCubeNetController] BUFFER OVERRUN ------------------");
            recordIntermittentError(error_types::network::bufferOverrun);
        }

        messageTimes[receivedMessageNumber] = std::chrono::steady_clock::time_point{};
        if (pendingAckCount > 0) --pendingAckCount;
    }

    if (pendingAckCount > 0) {
        // Clean up entries that have been pending for more than 1 second.
        for (auto& slot : messageTimes) {
            if (slot == std::chrono::steady_clock::time_point{}) continue;
            if ((now - slot) > std::chrono::seconds(1)) {
                slot = std::chrono::steady_clock::time_point{};
                if (pendingAckCount > 0) --pendingAckCount;
            }
        }

        if (pendingAckCount > 0) {
            const bool staleAcks = (now - lastAckTime) > std::chrono::seconds(1);
            const bool shouldLogWait =
                staleAcks &&
                (lastAckWarningTime == std::chrono::steady_clock::time_point{} ||
                 (now - lastAckWarningTime) > std::chrono::seconds(1));
            if (shouldLogWait) {
                // Find the oldest pending entry for diagnostics.
                auto oldestTime = now;
                for (const auto& slot : messageTimes) {
                    if (slot != std::chrono::steady_clock::time_point{} && slot < oldestTime) {
                        oldestTime = slot;
                    }
                }
                const auto oldestMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - oldestTime).count();
                logInfo("[LaserCubeNetController] waiting for ack",
                        "pending", pendingAckCount,
                        "oldest_ms", oldestMs);
                recordIntermittentError(error_types::network::packetLoss);
                lastAckWarningTime = now;
            }

            // When the ack ring is saturated and the oldest pending packet has
            // been stuck for long enough, the controller is no longer making
            // forward progress. Treat this as a real connection loss so the UI
            // goes red instead of staying on a soft warning indefinitely.
            auto oldestTime = now;
            for (const auto& slot : messageTimes) {
                if (slot != std::chrono::steady_clock::time_point{} && slot < oldestTime) {
                    oldestTime = slot;
                }
            }
            const bool ackQueueSaturated =
                pendingAckCount >= static_cast<int>(messageTimes.size());
            const auto ackStallDuration = now - oldestTime;
            if (ackQueueSaturated && ackStallDuration >= ackDisconnectThreshold) {
                logError("[LaserCubeNetController] ack stream stalled, marking connection lost",
                         "pending", pendingAckCount,
                         "oldest_ms",
                         std::chrono::duration_cast<std::chrono::milliseconds>(ackStallDuration).count());
                recordConnectionError(error_types::network::connectionLost);
                networkConnected.store(false, std::memory_order_relaxed);
                setConnectionState(false);
                return;
            }
        }
    }
}

int LaserCubeNetController::getTotalBufferCapacity() const {
    return pointBufferCapacity.load(std::memory_order_relaxed);
}

std::optional<core::BufferState> LaserCubeNetController::getBufferState() const {
    return buildBufferState(
        pointBufferCapacity.load(std::memory_order_relaxed),
        lastEstimatedBufferFullness.load(std::memory_order_relaxed));
}

} // namespace libera::lasercubenet
