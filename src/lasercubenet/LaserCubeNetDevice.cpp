#include "libera/lasercubenet/LaserCubeNetDevice.hpp"

#include "libera/core/ByteBuffer.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace libera::lasercubenet {
namespace {
inline std::uint16_t encodeCoord(float value) {
    const float clamped = std::clamp((value + 1.0f) * 0.5f, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::round(clamped * 0x0FFF));
}

inline std::uint16_t encodeColour(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::round(clamped * 0x0FFF));
}

inline std::uint16_t read_u16_le(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8);
}
}

LaserCubeNetDevice::LaserCubeNetDevice() {
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
    constexpr std::uint32_t kSlowPps = 30000;
    pps.store(info.status().pointRate, std::memory_order_relaxed);
    newPps.store(kSlowPps, std::memory_order_relaxed);
    maxPointRate.store(info.status().pointRateMax, std::memory_order_relaxed);
    pointBufferCapacity.store(info.status().bufferMax, std::memory_order_relaxed);

    lastAckTime = std::chrono::steady_clock::now();
    lastDataSentTime = std::chrono::steady_clock::time_point{};
    lastDataSentBufferSize = 0;
    lastReportedBufferFullness.store(0, std::memory_order_relaxed);

    if (!io) {
        io = net::shared_io_context();
    }

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
            const auto targetPps = newPps.load(std::memory_order_relaxed);
            const auto currentPps = pps.load(std::memory_order_relaxed);
            if (targetPps != currentPps) {
                if (sendPointRate(targetPps)) {
                    pps.store(targetPps, std::memory_order_relaxed);
                }
            }

            const bool dataSent = sendPointsToDac();
            if (dataSent) {
                if ((messageNumber % 25) == 0) {
                    //std::this_thread::sleep_for(10ms);
                }
            }

            checkAcks();
        }

        std::this_thread::sleep_for(1ms);
    }
}

void LaserCubeNetDevice::setPointRate(std::uint32_t pointRateValue) {
    core::LaserDeviceBase::setPointRate(pointRateValue);
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
    frameNumber++;

    const int minEstimatedBufferFullness =
        std::max(calculateBufferFullnessByTimeSent(), calculateBufferFullnessByTimeAcked());
    const int latencyPointAdjustment = 300;
    int maxPointsToAdd = std::max(0, getDacTotalPointBufferCapacity() - minEstimatedBufferFullness - latencyPointAdjustment);

    const int maxPointsInPacket = static_cast<int>(LaserCubeNetConfig::MAX_POINTS_PER_PACKET);

    if (maxPointsToAdd <= 0) {
        return true;
    }

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

    if (pointsToSend.size() > static_cast<std::size_t>(maxPointsToAdd)) {
        pointsToSend.resize(static_cast<std::size_t>(maxPointsToAdd));
    }

    core::ByteBuffer packet;
    packet.appendUInt8(LaserCubeNetConfig::CMD_SAMPLE_DATA);
    packet.appendUInt8(0x00);
    packet.appendUInt8(messageNumber);
    packet.appendUInt8(frameNumber);

    for (const auto& pt : pointsToSend) {
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

        if (lastPacketSentTime.time_since_epoch().count() != 0) {
            const auto gap = now - lastPacketSentTime;
            if (lastSendLogTime.time_since_epoch().count() == 0 ||
                (now - lastSendLogTime) > std::chrono::milliseconds(500)) {
                lastSendLogTime = now;
                const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(gap).count();
                logInfo("[LaserCubeNetDevice] send_gap_ms",
                        gapMs,
                        "sent",
                        pointsToSend.size(),
                        "est_full",
                        minEstimatedBufferFullness,
                        "max_add",
                        maxPointsToAdd,
                        "pps",
                        pps.load(std::memory_order_relaxed));
            }
        }
        lastPacketSentTime = now;
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

    std::array<std::uint8_t, 4> buffer{};
    libera::net::asio::ip::udp::endpoint sender;
    std::size_t received = 0;
    auto ec = dataSocket->recv_from(buffer.data(), buffer.size(), sender,
                                    received, std::chrono::milliseconds(1), false);
    if (!ec && received == 4) {
        if (buffer[0] == LaserCubeNetConfig::CMD_GET_RINGBUFFER_FREE) {
            const std::uint8_t receivedMessageNumber = buffer[1];
            const auto it = messageTimes.find(receivedMessageNumber);
            if (it != messageTimes.end()) {
                const auto now = std::chrono::steady_clock::now();
                lastAckTime = now;

                const std::uint16_t bufferSpace = read_u16_le(&buffer[2]);
                const int bufferFullness = getDacTotalPointBufferCapacity() - static_cast<int>(bufferSpace);
                lastReportedBufferFullness.store(bufferFullness, std::memory_order_relaxed);

                if (it->second >= lastDataSentTime) {
                    lastDataSentTime = it->second;
                    lastDataSentBufferSize = bufferFullness;
                }

                if (bufferSpace == 0) {
                    logInfo("[LaserCubeNetDevice] BUFFER OVERRUN ------------------");
                }

                messageTimes.erase(it);
            }
        } else {
            logInfo("[LaserCubeNetDevice] DIFFERENT RESPONSE", static_cast<int>(buffer[0]));
        }
    } else if (received > 0) {
        logInfo("[LaserCubeNetDevice] message received is greater than 4 bytes", received);
    }

    if (!messageTimes.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = messageTimes.begin(); it != messageTimes.end(); ) {
            const auto delay = now - it->second;
            if (delay > std::chrono::seconds(10)) {
                it = messageTimes.erase(it);
                lastDroppedPacketTime = now;
            } else {
                ++it;
            }
        }
    }
}

int LaserCubeNetDevice::calculateBufferFullnessByTimeSent() {
    if (lastDataSentTime.time_since_epoch().count() == 0) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastDataSentTime).count();
    const auto currentPps = static_cast<double>(pps.load(std::memory_order_relaxed));
    if (currentPps <= 0.0) {
        return 0;
    }
    const double consumed = (static_cast<double>(elapsed) / 1000000.0) * currentPps;
    const int remaining = static_cast<int>(static_cast<double>(lastDataSentBufferSize) - consumed);
    return std::max(0, remaining);
}

int LaserCubeNetDevice::calculateBufferFullnessByTimeAcked() {
    if (lastAckTime.time_since_epoch().count() == 0) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastAckTime).count();
    const auto currentPps = static_cast<double>(pps.load(std::memory_order_relaxed));
    if (currentPps <= 0.0) {
        return 0;
    }
    const double consumed = (static_cast<double>(elapsed) / 1000000.0) * currentPps;
    const int remaining = static_cast<int>(static_cast<double>(lastReportedBufferFullness.load(std::memory_order_relaxed)) - consumed);
    return std::max(0, remaining);
}

int LaserCubeNetDevice::getDacTotalPointBufferCapacity() const {
    return pointBufferCapacity.load(std::memory_order_relaxed);
}

} // namespace libera::lasercubenet
