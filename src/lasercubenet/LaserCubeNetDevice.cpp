#include "libera/lasercubenet/LaserCubeNetDevice.hpp"

#include "libera/core/BufferEstimator.hpp"
#include "libera/core/ByteBuffer.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>
#include <vector>

namespace libera::lasercubenet {
namespace {
inline std::uint16_t encodeCoord(float value) {
    // LaserCube expects 12-bit unsigned coordinates (0..0x0FFF) mapped from -1..1.
    const float clamped = std::clamp((value + 1.0f) * 0.5f, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::round(clamped * 0x0FFF));
}

inline std::uint16_t encodeColour(float value) {
    // LaserCube uses 12-bit unsigned colour channels.
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::round(clamped * 0x0FFF));
}

inline std::uint16_t read_u16_le(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}

double percentileFromSortedSamples(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty()) {
        return 0.0;
    }

    const double clampedPercentile = std::clamp(percentile, 0.0, 1.0);
    const double rawIndex = clampedPercentile * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(rawIndex));
    const auto upper = static_cast<std::size_t>(std::ceil(rawIndex));
    if (lower == upper) {
        return sorted[lower];
    }
    const double weight = rawIndex - static_cast<double>(lower);
    return sorted[lower] + ((sorted[upper] - sorted[lower]) * weight);
}
}

LaserCubeNetDevice::LaserCubeNetDevice() {
    // Reuse the shared IO context so sockets share the same network thread.
    io = net::shared_io_context();
}

LaserCubeNetDevice::LaserCubeNetDevice(LaserCubeNetDeviceInfo info)
    : LaserCubeNetDevice() {
    ipAddress = info.ipAddress();
}

LaserCubeNetDevice::~LaserCubeNetDevice() {
    stop();
    close();
}

libera::expected<void> LaserCubeNetDevice::connect(const LaserCubeNetDeviceInfo& info) {
    ipAddress = info.ipAddress();
    // Start with the device-reported point rate; callers can override via setPointRate().
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

void LaserCubeNetDevice::close() {
    networkConnected.store(false, std::memory_order_relaxed);
    if (dataSocket) {
        dataSocket->close();
    }
    if (commandSocket) {
        commandSocket->close();
    }
}

void LaserCubeNetDevice::run() {
    using namespace std::chrono_literals;

    while (running.load()) {
        if (networkConnected.load(std::memory_order_relaxed)) {
            // Push point-rate changes down to the device when requested.
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

        // Avoid a busy loop when the device cannot accept more data yet.
        std::this_thread::sleep_for(1ms);
    }
}

void LaserCubeNetDevice::setPointRate(std::uint32_t pointRateValue) {
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

bool LaserCubeNetDevice::sendPointsToDac() {
    // Advance a notional frame index to mirror the LaserDockNet protocol.
    frameNumber++;

    // Estimate how full the device buffer is right now.
    const int minEstimatedBufferFullness =
        std::max(calculateBufferFullnessByTimeSent(), calculateBufferFullnessByTimeAcked());
    lastEstimatedBufferFullness.store(minEstimatedBufferFullness, std::memory_order_relaxed);
    const int latencyPointAdjustment = 300;
    int maxPointsToAdd = std::max(0, getDacTotalPointBufferCapacity() - minEstimatedBufferFullness - latencyPointAdjustment);

    const int maxPointsInPacket = static_cast<int>(LaserCubeNetConfig::MAX_POINTS_PER_PACKET);

    if (maxPointsToAdd <= 0) {
        return true;
    }

    // Only request points when we have space for a full packet.
    if (maxPointsToAdd < maxPointsInPacket) {
        return true;
    }

    if (maxPointsToAdd > maxPointsInPacket) {
        maxPointsToAdd = maxPointsInPacket;
    }

    core::PointFillRequest request{};
    const int minPointsToAdd = maxPointsToAdd;
    request.minimumPointsRequired = static_cast<std::size_t>(minPointsToAdd);
    request.maximumPointsRequired = static_cast<std::size_t>(maxPointsToAdd);

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
        packet.appendUInt16(encodeCoord(pt.x));
        packet.appendUInt16(encodeCoord(pt.y));
        packet.appendUInt16(encodeColour(pt.r));
        packet.appendUInt16(encodeColour(pt.g));
        packet.appendUInt16(encodeColour(pt.b));
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

bool LaserCubeNetDevice::sendPointRate(std::uint32_t rate) {
    core::ByteBuffer payload;
    payload.appendUInt32(rate);
    return sendCommand(LaserCubeNetConfig::CMD_SET_ILDA_RATE, payload.data(), payload.size());
}

bool LaserCubeNetDevice::sendData(const std::uint8_t* buffer, std::size_t size) {
    if (!dataSocket || !buffer || size == 0) {
        return false;
    }
    auto ec = dataSocket->send_to(buffer, size, dataEndpoint, std::chrono::milliseconds(50));
    if (ec) {
        logError("[LaserCubeNetDevice] Failed to send data", ec.message());
        return false;
    }
    return true;
}

bool LaserCubeNetDevice::sendCommand(std::uint8_t cmd, const std::uint8_t* payload, std::size_t size) {
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
        logError("[LaserCubeNetDevice] command send failed", ec.message());
        return false;
    }
    return true;
}

void LaserCubeNetDevice::checkAcks() {
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
            logInfo("[LaserCubeNetDevice] ack receive failed", ec.message());
            break;
        }
        ++polls;

        if (sender.address() != dataEndpoint.address() || sender.port() != dataEndpoint.port()) {
            const auto now = std::chrono::steady_clock::now();
            if (lastUnexpectedAckSenderLogTime == std::chrono::steady_clock::time_point{} ||
                (now - lastUnexpectedAckSenderLogTime) > std::chrono::seconds(1)) {
                logInfo("[LaserCubeNetDevice] ignoring ack from unexpected sender",
                        sender.address().to_string(), sender.port(),
                        "expected", dataEndpoint.address().to_string(), dataEndpoint.port());
                lastUnexpectedAckSenderLogTime = now;
            }
            continue;
        }

        if (received != 4) {
            if (received > 0) {
                logInfo("[LaserCubeNetDevice] ack packet unexpected size", received);
            }
            continue;
        }

        if (buffer[0] != LaserCubeNetConfig::CMD_GET_RINGBUFFER_FREE) {
            logInfo("[LaserCubeNetDevice] DIFFERENT RESPONSE", static_cast<int>(buffer[0]));
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

        const std::uint16_t bufferSpace = read_u16_le(&buffer[2]);
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
            logInfo("[LaserCubeNetDevice] BUFFER OVERRUN ------------------");
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
                logInfo("[LaserCubeNetDevice] waiting for ack",
                        "pending", static_cast<int>(messageTimes.size()),
                        "oldest_ms", oldestMs);
                lastAckWarningTime = now;
            }
        }
    }
}

int LaserCubeNetDevice::calculateBufferFullnessByTimeSent() {
    const auto estimate = core::BufferEstimator::estimateFromAnchor(
        lastDataSentBufferSize,
        lastDataSentTime,
        pps.load(std::memory_order_relaxed));
    if (!estimate.projected) {
        return 0;
    }
    return estimate.bufferFullness;
}

int LaserCubeNetDevice::calculateBufferFullnessByTimeAcked() {
    const auto estimate = core::BufferEstimator::estimateFromAnchor(
        lastReportedBufferFullness.load(std::memory_order_relaxed),
        lastAckTime,
        pps.load(std::memory_order_relaxed));
    if (!estimate.projected) {
        return 0;
    }
    return estimate.bufferFullness;
}

int LaserCubeNetDevice::getDacTotalPointBufferCapacity() const {
    return pointBufferCapacity.load(std::memory_order_relaxed);
}

std::optional<core::DacBufferState> LaserCubeNetDevice::getBufferState() const {
    const int total = pointBufferCapacity.load(std::memory_order_relaxed);
    if (total <= 0) {
        return std::nullopt;
    }

    const int fullness = std::clamp(
        lastEstimatedBufferFullness.load(std::memory_order_relaxed),
        0,
        total);

    core::DacBufferState state;
    state.pointsInBuffer = fullness;
    state.totalBufferPoints = total;
    return state;
}

void LaserCubeNetDevice::recordLatencySample(std::chrono::steady_clock::duration sample) {
    const double sampleMs =
        std::chrono::duration<double, std::milli>(sample).count();
    if (sampleMs < 0.0) {
        return;
    }

    std::lock_guard<std::mutex> lock(latencySamplesMutex);
    latencySamplesMs.push_back(sampleMs);
    while (latencySamplesMs.size() > latencySampleWindow) {
        latencySamplesMs.pop_front();
    }
}

std::optional<core::DacLatencyStats> LaserCubeNetDevice::getLatencyStats() const {
    std::vector<double> sortedSamples;
    {
        std::lock_guard<std::mutex> lock(latencySamplesMutex);
        if (latencySamplesMs.empty()) {
            return std::nullopt;
        }
        sortedSamples.assign(latencySamplesMs.begin(), latencySamplesMs.end());
    }

    std::sort(sortedSamples.begin(), sortedSamples.end());

    core::DacLatencyStats stats;
    stats.sampleCount = sortedSamples.size();
    stats.p50Ms = percentileFromSortedSamples(sortedSamples, 0.50);
    stats.p95Ms = percentileFromSortedSamples(sortedSamples, 0.95);
    stats.p99Ms = percentileFromSortedSamples(sortedSamples, 0.99);
    stats.maxMs = sortedSamples.back();
    return stats;
}

} // namespace libera::lasercubenet
