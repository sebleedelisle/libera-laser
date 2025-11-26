#pragma once

#include "libera/core/LaserDevice.hpp"
#include "libera/core/Expected.hpp"
#include "libera/core/ByteBuffer.hpp"
#include "libera/lasercubenet/LaserCubeNetDeviceInfo.hpp"
#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/lasercubenet/LaserCubeNetStatus.hpp"
#include "libera/net/UdpSocket.hpp"
#include "libera/net/NetService.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace libera::lasercubenet {

class LaserCubeNetDevice : public core::LaserDevice {
public:
    LaserCubeNetDevice();
    explicit LaserCubeNetDevice(LaserCubeNetDeviceInfo info);
    ~LaserCubeNetDevice() override;

    libera::expected<void> connect(const LaserCubeNetDeviceInfo& info);
    void close();

protected:
    void run() override;
    void setPointRate(std::uint32_t pointRate) override;
    core::PointFillRequest buildFillRequest();

private:
    bool sendPointsBatch();
    bool sendCommand(std::uint8_t cmd, const std::uint8_t* payload, std::size_t size);
    void handleBufferAck(std::uint8_t messageId, std::uint16_t bufferFree);
    void handleStatusPacket(const std::uint8_t* data, std::size_t size);
    void startAckThread();
    void stopAckThread();
    void ackLoop();

    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<net::UdpSocket> dataSocket;
    std::unique_ptr<net::UdpSocket> commandSocket;
    net::udp::endpoint dataEndpoint;
    net::udp::endpoint commandEndpoint;

    std::string ipAddress;
    std::uint8_t messageNumber = 0;
    std::uint8_t frameNumber = 0;

    std::thread ackThread;
    std::atomic<bool> ackRunning{false};

    mutable std::mutex ackMutex;
    std::map<std::uint8_t, std::chrono::steady_clock::time_point> pendingAcks;
    std::atomic<int> reportedBufferFree{LaserCubeNetConfig::MAX_POINTS_PER_PACKET};
    std::atomic<int> bufferCapacity{LaserCubeNetConfig::MAX_POINTS_PER_PACKET};

    std::atomic<std::uint32_t> configuredPointRate{30000};
    std::chrono::steady_clock::time_point lastStatusRequest{};
    std::chrono::steady_clock::time_point lastSendTime{};
};

} // namespace libera::lasercubenet
