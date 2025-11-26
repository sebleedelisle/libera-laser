#include "libera/lasercubenet/LaserCubeNetDevice.hpp"

#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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

    const std::uint8_t enablePayload = 1;
    sendCommand(LaserCubeNetConfig::CMD_ENABLE_BUFFER_RESPONSE, &enablePayload, 1);
    sendCommand(LaserCubeNetConfig::CMD_SET_OUTPUT, &enablePayload, 1);

    core::ByteBuffer ratePayload;
    ratePayload.appendUInt32(configuredPointRate.load());
    sendCommand(LaserCubeNetConfig::CMD_SET_ILDA_RATE, ratePayload.data(), ratePayload.size());

    lastStatusRequest = std::chrono::steady_clock::now();
    startAckThread();
    return {};
}

void LaserCubeNetDevice::close() {
    stopAckThread();
    if (dataSocket) {
        dataSocket->close();
    }
    if (commandSocket) {
        commandSocket->close();
    }
}

void LaserCubeNetDevice::run() {
    using namespace std::chrono_literals;

    core::PointFillRequest request{};

    while (running.load()) {
        request = buildFillRequest();
        if (!requestPoints(request) || pointsToSend.empty()) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        if (!sendPointsBatch()) {
            std::this_thread::sleep_for(2ms);
        }
    }
}

core::PointFillRequest LaserCubeNetDevice::buildFillRequest() {
    core::PointFillRequest req{};

    const auto now = std::chrono::steady_clock::now();
    const std::uint32_t pps = configuredPointRate.load(std::memory_order_relaxed);
    const int capacity = bufferCapacity.load(std::memory_order_relaxed);
    int freeSpace = reportedBufferFree.load(std::memory_order_relaxed);
    freeSpace = std::max(0, std::min(freeSpace, capacity));

    constexpr auto MIN_BUFFER_MS = std::chrono::milliseconds(5);
    const std::size_t minPoints = pps == 0
        ? 0
        : static_cast<std::size_t>(std::ceil((pps / 1000.0) * MIN_BUFFER_MS.count()));

    req.minimumPointsRequired = std::min<std::size_t>(minPoints, freeSpace);
    req.maximumPointsRequired = freeSpace;

    const auto bufferedPoints = capacity - freeSpace;
    const double bufferedMillis = (pps == 0)
        ? 0.0
        : (static_cast<double>(bufferedPoints) * 1000.0) / static_cast<double>(pps);

    req.estimatedFirstPointRenderTime =
        now + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double, std::milli>(bufferedMillis));

    return req;
}

void LaserCubeNetDevice::setPointRate(std::uint32_t pointRate) {
    configuredPointRate.store(pointRate, std::memory_order_relaxed);
    core::ByteBuffer payload;
    payload.appendUInt32(pointRate);
    sendCommand(LaserCubeNetConfig::CMD_SET_ILDA_RATE, payload.data(), payload.size());
}

bool LaserCubeNetDevice::sendPointsBatch() {
    if (!dataSocket || pointsToSend.empty()) {
        return false;
    }

    int bufferFree = reportedBufferFree.load(std::memory_order_relaxed);
    if (bufferFree <= 0) {
        return false;
    }

    std::size_t offset = 0;
    std::size_t totalSent = 0;
    while (offset < pointsToSend.size() && bufferFree > 0) {
        const std::size_t chunk = std::min<std::size_t>({LaserCubeNetConfig::MAX_POINTS_PER_PACKET,
                                                         pointsToSend.size() - offset,
                                                         static_cast<std::size_t>(bufferFree)});
        if (chunk == 0) {
            break;
        }

        core::ByteBuffer packet;
        packet.appendUInt8(LaserCubeNetConfig::CMD_SAMPLE_DATA);
        packet.appendUInt8(0x00);
        packet.appendUInt8(messageNumber);
        packet.appendUInt8(frameNumber);

        for (std::size_t i = 0; i < chunk; ++i) {
            const auto& pt = pointsToSend[offset + i];
            packet.appendUInt16(encodeCoord(pt.x));
            packet.appendUInt16(encodeCoord(pt.y));
            packet.appendUInt16(encodeColour(pt.r));
            packet.appendUInt16(encodeColour(pt.g));
            packet.appendUInt16(encodeColour(pt.b));
        }

        auto ec = dataSocket->send_to(packet.data(), packet.size(), dataEndpoint,
                                      std::chrono::milliseconds(50));
        if (ec) {
            logError("[LaserCubeNetDevice] Failed to send data", ec.message());
            return false;
        }

        {
            std::lock_guard lock(ackMutex);
            pendingAcks[messageNumber] = std::chrono::steady_clock::now();
        }

        ++messageNumber;
        offset += chunk;
        bufferFree -= static_cast<int>(chunk);
        totalSent += chunk;
    }

    ++frameNumber;
    pointsToSend.clear();

    if (totalSent > 0) {
        int prev = reportedBufferFree.load();
        int updated;
        do {
            updated = std::max(0, prev - static_cast<int>(totalSent));
        } while (!reportedBufferFree.compare_exchange_weak(prev, updated));
    }
    return true;
}

bool LaserCubeNetDevice::sendCommand(std::uint8_t cmd, const std::uint8_t* payload, std::size_t size) {
    if (!commandSocket) {
        return false;
    }
    if (!payload && size > 0) {
        return false;
    }

    core::ByteBuffer buffer;
    buffer.appendUInt8(cmd);
    for (std::size_t i = 0; i < size; ++i) {
        buffer.appendUInt8(payload ? payload[i] : 0);
    }

    auto ec = commandSocket->send_to(buffer.data(), buffer.size(), commandEndpoint,
                                     std::chrono::milliseconds(200));
    if (ec) {
        logError("[LaserCubeNetDevice] command send failed", ec.message());
        return false;
    }
    return true;
}

void LaserCubeNetDevice::handleBufferAck(std::uint8_t messageId, std::uint16_t bufferFree) {
    std::lock_guard lock(ackMutex);
    pendingAcks.erase(messageId);
    reportedBufferFree.store(static_cast<int>(bufferFree), std::memory_order_relaxed);
}

void LaserCubeNetDevice::handleStatusPacket(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 64) {
        return;
    }
    const std::uint8_t* payload = data;
    std::size_t payloadSize = size;
    if (payloadSize > 1 && payload[0] == LaserCubeNetConfig::CMD_GET_FULL_INFO) {
        ++payload;
        --payloadSize;
    }
    if (auto status = LaserCubeNetStatus::parse(payload, payloadSize)) {
        bufferCapacity.store(status->bufferMax, std::memory_order_relaxed);
        reportedBufferFree.store(status->bufferFree, std::memory_order_relaxed);
    }
}

void LaserCubeNetDevice::startAckThread() {
    ackRunning.store(true);
    ackThread = std::thread([this]{ ackLoop(); });
}

void LaserCubeNetDevice::stopAckThread() {
    ackRunning.store(false);
    if (ackThread.joinable()) {
        ackThread.join();
    }
}

void LaserCubeNetDevice::ackLoop() {
    using namespace std::chrono_literals;
    constexpr auto dataTimeout = std::chrono::milliseconds(250);
    constexpr auto statusTimeout = std::chrono::milliseconds(250);
    std::array<std::uint8_t, 128> buffer{};
    libera::net::asio::ip::udp::endpoint sender;
    auto nextStatusRequest = std::chrono::steady_clock::now();

    while (ackRunning.load()) {
        if (dataSocket) {
            std::size_t received = 0;
            auto ec = dataSocket->recv_from(buffer.data(), buffer.size(), sender,
                                            received, dataTimeout, false);
            if (!ec && received >= 1) {
                if (received == 4 && buffer[0] == LaserCubeNetConfig::CMD_GET_RINGBUFFER_FREE) {
                    const std::uint8_t msgId = buffer[1];
                    const std::uint16_t free = static_cast<std::uint16_t>(buffer[2]) |
                                               (static_cast<std::uint16_t>(buffer[3]) << 8);
                    handleBufferAck(msgId, free);
                }
            }
        }

        if (commandSocket) {
            if (std::chrono::steady_clock::now() >= nextStatusRequest) {
                sendCommand(LaserCubeNetConfig::CMD_GET_FULL_INFO, nullptr, 0);
                lastStatusRequest = std::chrono::steady_clock::now();
                nextStatusRequest = lastStatusRequest + std::chrono::seconds(1);
            }

            std::size_t received = 0;
            auto ec = commandSocket->recv_from(buffer.data(), buffer.size(), sender,
                                               received, statusTimeout, false);
            if (!ec && received >= 64) {
                handleStatusPacket(buffer.data(), received);
            }
        }

        {
            std::lock_guard lock(ackMutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = pendingAcks.begin(); it != pendingAcks.end(); ) {
                if (now - it->second > std::chrono::seconds(1)) {
                    it = pendingAcks.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

} // namespace libera::lasercubenet
