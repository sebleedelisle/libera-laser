#include "libera/lasercubenet/LaserCubeNetController.hpp"

#include "libera/core/ByteRead.hpp"
#include "libera/core/ByteBuffer.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <thread>

namespace libera::lasercubenet {

LaserCubeNetController::LaserCubeNetController() {
    // Reuse the shared IO context so sockets share the same network thread.
    io = net::shared_io_context();
}

LaserCubeNetController::LaserCubeNetController(LaserCubeNetControllerInfo info)
    : LaserCubeNetController() {
    ipAddress = info.ipAddress();
}

LaserCubeNetController::~LaserCubeNetController() {
    stop();
    close();
}

libera::expected<void> LaserCubeNetController::connect(const LaserCubeNetControllerInfo& info) {
    ipAddress = info.ipAddress();
    // Start with the controller-reported point rate; callers can override via setPointRate().
    pps.store(info.status().pointRate, std::memory_order_relaxed);
    newPps.store(info.status().pointRate, std::memory_order_relaxed);
    maxPointRate.store(info.status().pointRateMax, std::memory_order_relaxed);
    pointBufferCapacity.store(info.status().bufferMax, std::memory_order_relaxed);

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

    // Data socket sends point packets; command socket handles control/status.
    dataSocket = std::make_unique<net::UdpSocket>(*io);
    commandSocket = std::make_unique<net::UdpSocket>(*io);

    if (auto ec = dataSocket->open_v4()) {
        return libera::unexpected(ec);
    }
    if (auto ec = dataSocket->bind_any(0)) {
        return libera::unexpected(ec);
    }
    if (auto ec = commandSocket->open_v4()) {
        return libera::unexpected(ec);
    }
    if (auto ec = commandSocket->bind_any(0)) {
        return libera::unexpected(ec);
    }

    std::error_code ecAddr;
    auto address = libera::net::asio::ip::make_address(ipAddress, ecAddr);
    if (ecAddr) {
        return libera::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    dataEndpoint = libera::net::asio::ip::udp::endpoint(address, LaserCubeNetConfig::DATA_PORT);
    commandEndpoint = libera::net::asio::ip::udp::endpoint(address, LaserCubeNetConfig::COMMAND_PORT);

    networkConnected.store(true, std::memory_order_relaxed);
    return {};
}

void LaserCubeNetController::close() {
    networkConnected.store(false, std::memory_order_relaxed);
    if (dataSocket) {
        dataSocket->close();
    }
    if (commandSocket) {
        commandSocket->close();
    }
}

void LaserCubeNetController::run() {
    using namespace std::chrono_literals;

    while (running.load()) {
        if (networkConnected.load(std::memory_order_relaxed)) {
            // Push point-rate changes down to the controller when requested.
            const auto targetPps = newPps.load(std::memory_order_relaxed);
            const auto currentPps = pps.load(std::memory_order_relaxed);
            if (targetPps != currentPps) {
                if (sendPointRate(targetPps)) {
                    pps.store(targetPps, std::memory_order_relaxed);
                }
            }

            // Send at most one packet per loop; pacing is handled by buffer estimation.
            (void)sendPointsToDac();
            checkAcks();
        }

        // Avoid a busy loop when the controller cannot accept more data yet.
        std::this_thread::sleep_for(1ms);
    }
}

void LaserCubeNetController::setPointRate(std::uint32_t pointRateValue) {
    core::LaserControllerStreaming::setPointRate(pointRateValue);
    if (pointRateValue > maxPointRate.load(std::memory_order_relaxed)) {
        pointRateValue = maxPointRate.load(std::memory_order_relaxed);
    }
    if (!running.load()) {
        pps.store(pointRateValue, std::memory_order_relaxed);
        newPps.store(pointRateValue, std::memory_order_relaxed);
    } else {
        newPps.store(pointRateValue, std::memory_order_relaxed);
    }
}

bool LaserCubeNetController::sendPointsToDac() {
    // Advance a notional frame index to mirror the LaserDockNet protocol.
    frameNumber++;

    const auto activePps = pps.load(std::memory_order_relaxed);

    // Estimate how full the controller buffer is right now.
    const int minEstimatedBufferFullness =
        std::max(
            calculateBufferFullnessFromAnchor(
                lastDataSentBufferSize,
                lastDataSentTime,
                activePps,
                0),
            calculateBufferFullnessFromAnchor(
                lastReportedBufferFullness.load(std::memory_order_relaxed),
                lastAckTime,
                activePps,
                0));
    lastEstimatedBufferFullness.store(minEstimatedBufferFullness, std::memory_order_relaxed);
    const int latencyPointAdjustment = 300;
    int maxPointsToAdd = std::max(
        0,
        getDacTotalPointBufferCapacity() - minEstimatedBufferFullness - latencyPointAdjustment);

    const int maxPointsInPacket = static_cast<int>(LaserCubeNetConfig::MAX_POINTS_PER_PACKET);

    if (maxPointsToAdd <= 0) {
        return true;
    }

    if (maxPointsToAdd > maxPointsInPacket) {
        maxPointsToAdd = maxPointsInPacket;
    }

    core::PointFillRequest request{};
    // Ask for up to the available room, but keep the hard minimum small so a
    // frame can finish naturally instead of being padded to packet size.
    request.minimumPointsRequired = 1;
    request.maximumPointsRequired = static_cast<std::size_t>(maxPointsToAdd);
    const auto renderLead = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(
            pointsToMillis(static_cast<std::size_t>(minEstimatedBufferFullness), activePps)));
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now() + renderLead;

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
        // Each component is clamped and quantized to 12 bits.
        packet.appendUInt16(encodeUnsigned12FromSignedUnit(pt.x));
        packet.appendUInt16(encodeUnsigned12FromSignedUnit(pt.y));
        packet.appendUInt16(encodeUnsigned12FromUnit(pt.r));
        packet.appendUInt16(encodeUnsigned12FromUnit(pt.g));
        packet.appendUInt16(encodeUnsigned12FromUnit(pt.b));
    }

    const bool success = sendData(packet.data(), packet.size());
    if (success) {
        const auto now = std::chrono::steady_clock::now();
        messageTimes[messageNumber] = now;
        lastDataSentTime = now;
        lastDataSentBufferSize = minEstimatedBufferFullness + static_cast<int>(pointsToSend.size());
    }

    messageNumber++;

    return success;
}

bool LaserCubeNetController::sendPointRate(std::uint32_t rate) {
    core::ByteBuffer payload;
    payload.appendUInt32(rate);
    return sendCommand(LaserCubeNetConfig::CMD_SET_ILDA_RATE, payload.data(), payload.size());
}

bool LaserCubeNetController::sendData(const std::uint8_t* buffer, std::size_t size) {
    if (!dataSocket || !buffer || size == 0) {
        return false;
    }
    auto ec = dataSocket->send_to(buffer, size, dataEndpoint, std::chrono::milliseconds(50));
    if (ec) {
        logError("[LaserCubeNetController] Failed to send data", ec.message());
        return false;
    }
    return true;
}

bool LaserCubeNetController::sendCommand(std::uint8_t cmd, const std::uint8_t* payload, std::size_t size) {
    if (!commandSocket) {
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
    while (polls < maxAckPollsPerCall) {
        // Each ack is a 4-byte response carrying the free-space count.
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
            break;
        }
        ++polls;

        if (sender.address() != dataEndpoint.address() || sender.port() != dataEndpoint.port()) {
            const auto now = std::chrono::steady_clock::now();
            if (lastUnexpectedAckSenderLogTime == std::chrono::steady_clock::time_point{} ||
                (now - lastUnexpectedAckSenderLogTime) > std::chrono::seconds(1)) {
                logInfo("[LaserCubeNetController] ignoring ack from unexpected sender",
                        sender.address().to_string(), sender.port(),
                        "expected", dataEndpoint.address().to_string(), dataEndpoint.port());
                lastUnexpectedAckSenderLogTime = now;
            }
            continue;
        }

        if (received != 4) {
            if (received > 0) {
                logInfo("[LaserCubeNetController] ack packet unexpected size", received);
            }
            continue;
        }

        if (buffer[0] != LaserCubeNetConfig::CMD_GET_RINGBUFFER_FREE) {
            logInfo("[LaserCubeNetController] DIFFERENT RESPONSE", static_cast<int>(buffer[0]));
            continue;
        }

        // Ack layout (byte offsets):
        //  0: CMD_GET_RINGBUFFER_FREE (0x8A)
        //  1: messageNumber echoed back
        //  2: free-space LSB
        //  3: free-space MSB
        const std::uint8_t receivedMessageNumber = buffer[1];
        const auto it = messageTimes.find(receivedMessageNumber);
        if (it == messageTimes.end()) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        recordLatencySample(now - it->second);
        lastAckTime = now;
        lastAckWarningTime = std::chrono::steady_clock::time_point{};

        const std::uint16_t bufferSpace = core::bytes::readLe16(&buffer[2]);
        // Convert free-space to fullness (capacity - free).
        const int bufferFullness = getDacTotalPointBufferCapacity() - static_cast<int>(bufferSpace);
        lastReportedBufferFullness.store(bufferFullness, std::memory_order_relaxed);
        lastEstimatedBufferFullness.store(bufferFullness, std::memory_order_relaxed);

        // If this ack corresponds to the most recent send, anchor the time-sent estimate.
        if (it->second >= lastDataSentTime) {
            lastDataSentTime = it->second;
            lastDataSentBufferSize = bufferFullness;
        }

        if (bufferSpace == 0) {
            logInfo("[LaserCubeNetController] BUFFER OVERRUN ------------------");
        }

        messageTimes.erase(it);
    }

    if (!messageTimes.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = messageTimes.begin(); it != messageTimes.end(); ) {
            const auto delay = now - it->second;
            if (delay > std::chrono::seconds(10)) {
                it = messageTimes.erase(it);
            } else {
                ++it;
            }
        }

        if (!messageTimes.empty()) {
            const bool staleAcks = (now - lastAckTime) > std::chrono::seconds(1);
            const bool shouldLogWait =
                staleAcks &&
                (lastAckWarningTime == std::chrono::steady_clock::time_point{} ||
                 (now - lastAckWarningTime) > std::chrono::seconds(1));
            if (shouldLogWait) {
                const auto oldestPending = now - messageTimes.begin()->second;
                const auto oldestMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(oldestPending).count();
                logInfo("[LaserCubeNetController] waiting for ack",
                        "pending", static_cast<int>(messageTimes.size()),
                        "oldest_ms", oldestMs);
                lastAckWarningTime = now;
            }
        }
    }
}

int LaserCubeNetController::getDacTotalPointBufferCapacity() const {
    return pointBufferCapacity.load(std::memory_order_relaxed);
}

std::optional<core::DacBufferState> LaserCubeNetController::getBufferState() const {
    return buildBufferState(
        pointBufferCapacity.load(std::memory_order_relaxed),
        lastEstimatedBufferFullness.load(std::memory_order_relaxed));
}

} // namespace libera::lasercubenet
