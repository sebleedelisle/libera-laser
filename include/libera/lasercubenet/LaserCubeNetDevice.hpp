#pragma once

#include "libera/core/Expected.hpp"
#include "libera/core/LaserDevice.hpp"
#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/lasercubenet/LaserCubeNetDeviceInfo.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <chrono>

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

private:
    bool sendPointsToDac();
    bool sendPointRate(std::uint32_t rate);
    bool sendData(const std::uint8_t* buffer, std::size_t size);
    bool sendCommand(std::uint8_t cmd, const std::uint8_t* payload, std::size_t size);
    void checkAcks();

    int calculateBufferFullnessByTimeSent();
    int calculateBufferFullnessByTimeAcked();
    int getDacTotalPointBufferCapacity() const;

    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<net::UdpSocket> dataSocket;
    std::unique_ptr<net::UdpSocket> commandSocket;
    net::udp::endpoint dataEndpoint;
    net::udp::endpoint commandEndpoint;

    std::string ipAddress;

    std::atomic<int> pointBufferCapacity{1000};
    std::atomic<std::uint32_t> pps{30000};
    std::atomic<std::uint32_t> newPps{30000};
    std::atomic<std::uint32_t> maxPointRate{60000};
    std::atomic<bool> networkConnected{false};

    std::uint8_t messageNumber{0};
    std::uint8_t frameNumber{0};

    // Track when each packet was sent so we can map acks to send times.
    std::map<std::uint8_t, std::chrono::steady_clock::time_point> messageTimes;

    // Timing helpers for buffer estimation and health tracking.
    std::chrono::steady_clock::time_point lastAckTime{};
    std::chrono::steady_clock::time_point lastDataSentTime{};
    int lastDataSentBufferSize{0};
    std::atomic<int> lastReportedBufferFullness{0};

    static constexpr int minPacketDataSize = 128;
};

} // namespace libera::lasercubenet
