#pragma once

#include "libera/core/Expected.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/lasercubenet/LaserCubeNetConfig.hpp"
#include "libera/lasercubenet/LaserCubeNetControllerInfo.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/UdpSocket.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <chrono>
#include <optional>

namespace libera::lasercubenet {

class LaserCubeNetController : public core::LaserController {
public:
    LaserCubeNetController();
    explicit LaserCubeNetController(LaserCubeNetControllerInfo info);
    ~LaserCubeNetController() override;

    libera::expected<void> connect(const LaserCubeNetControllerInfo& info);
    void close();
    std::optional<core::BufferState> getBufferState() const override;

protected:
    void run() override;
    void setPointRate(std::uint32_t pointRate) override;

private:
    bool sendPoints();
    bool sendPointRate(std::uint32_t rate);
    bool sendData(const std::uint8_t* buffer, std::size_t size);
    bool sendCommand(std::uint8_t cmd, const std::uint8_t* payload, std::size_t size);
    void checkAcks();

    int getTotalBufferCapacity() const;

    std::shared_ptr<asio::io_context> io;
    std::unique_ptr<net::UdpSocket> dataSocket;
    std::unique_ptr<net::UdpSocket> commandSocket;
    net::udp::endpoint dataEndpoint;
    net::udp::endpoint commandEndpoint;

    std::string ipAddress;

    std::atomic<int> pointBufferCapacity{1000};
    std::atomic<std::uint32_t> currentPointRate{30000};
    std::atomic<std::uint32_t> pendingPointRate{30000};
    std::atomic<std::uint32_t> maxPointRate{60000};
    std::atomic<bool> networkConnected{false};

    std::uint8_t messageNumber{0};
    std::uint8_t frameNumber{0};

    // Fixed-size array indexed by messageNumber (uint8_t wraps at 256).
    // A default-constructed time_point (epoch) means "slot empty".
    std::array<std::chrono::steady_clock::time_point, 256> messageTimes{};
    int pendingAckCount{0};

    // Timing helpers for buffer estimation and health tracking.
    std::chrono::steady_clock::time_point lastAckTime{};
    std::chrono::steady_clock::time_point lastAckWarningTime{};
    std::chrono::steady_clock::time_point lastUnexpectedAckSenderLogTime{};
    std::chrono::steady_clock::time_point lastDataSentTime{};
    int lastDataSentBufferSize{0};
    std::atomic<int> lastReportedBufferFullness{0};
    std::atomic<int> lastEstimatedBufferFullness{0};
};

} // namespace libera::lasercubenet
