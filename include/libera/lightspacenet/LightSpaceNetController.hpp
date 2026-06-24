#pragma once

#include "libera/core/Expected.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/lightspacenet/LightSpaceNetControllerInfo.hpp"
#include "libera/lightspacenet/LightSpaceNetPacket.hpp"
#include "libera/lightspacenet/LightSpaceNetStatus.hpp"
#include "libera/net/TcpClient.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace libera::lightspacenet {

class LightSpaceNetController : public core::LaserController {
public:
    LightSpaceNetController();
    explicit LightSpaceNetController(LightSpaceNetControllerInfo info);
    ~LightSpaceNetController() override;

    libera::expected<void> connect(const LightSpaceNetControllerInfo& info);
    void close();
    void updateDiscoveredStatus(const LightSpaceNetStatus& status);

protected:
    void run() override;
    void setPointRate(std::uint32_t pointRate) override;

private:
    libera::expected<void> connectToStatus(const LightSpaceNetStatus& status);
    bool reconnectToLatestStatus();

    bool sendPacket(const std::vector<std::uint8_t>& packet,
                    std::chrono::milliseconds timeout);
    bool sendControlCommand(std::uint8_t commandWord,
                            const std::vector<std::uint8_t>& packet);
    bool sendReliableCommand(std::uint8_t commandWord,
                             const std::vector<std::uint8_t>& packet);
    bool waitForCommandAck(std::uint8_t commandWord,
                           std::chrono::steady_clock::time_point deadline);
    bool hasTcpConnection() const;
    bool handleParsedIncomingPacket(const LightSpaceNetPacket& packet,
                                    std::uint8_t expectedAckCommand,
                                    bool& ackMatched);
    bool pollTcpIncomingPackets(std::uint8_t expectedAckCommand,
                                bool& ackMatched);
    bool drainTcpPacketBuffer(std::uint8_t expectedAckCommand,
                              bool& ackMatched);
    void pollIncomingPackets();
    void sendHeartbeatIfDue();
    void syncPointRate();
    void syncLaserState();
    void sendBlankPatternForShutdown();
    bool sendFramePattern();
    void recordTimingSample(std::size_t sentPointCount,
                            std::uint32_t activePointRate);

    std::unique_ptr<net::TcpClient> tcpClient;
    net::tcp::endpoint tcpEndpoint;

    std::string ipAddress;

    std::atomic<bool> networkConnected{false};
    std::atomic<bool> reconnectRequested{false};

    mutable std::mutex latestStatusMutex;
    std::optional<LightSpaceNetStatus> latestStatus;

    std::chrono::steady_clock::time_point lastHeartbeatSentTime{};
    std::chrono::steady_clock::time_point lastHeartbeatReplyTime{};

    std::uint32_t lastSentPointRate{0};
    bool pointRatePushNeeded{true};
    std::chrono::steady_clock::time_point nextPointRateSyncTime{};

    bool lastSentArmed{false};
    bool laserStatePushNeeded{true};
    std::chrono::steady_clock::time_point nextLaserStateSyncTime{};

    LightSpaceNetCoordinateOptions coordinateOptions;
    bool commandAckRequired{true};
    bool strictHeartbeat{false};
    bool timingLogEnabled{false};
    bool heartbeatTimeoutLogged{false};
    std::chrono::milliseconds patternUpdateInterval{
        LightSpaceNetConfig::DEFAULT_PATTERN_UPDATE_INTERVAL};
    std::chrono::steady_clock::time_point lastPatternSentTime{};
    std::chrono::steady_clock::time_point lastIncomingPollTime{};

    std::size_t patternPointLimit{LightSpaceNetConfig::DEFAULT_PATTERN_POINTS};
    std::size_t lastSentPacketPointCount{0};
    std::size_t lastSentPacketBytes{0};
    std::uint64_t currentPointIndex{0};

    std::vector<std::uint8_t> tcpReceiveBuffer;
    std::chrono::steady_clock::time_point timingLogWindowStart{};
    std::uint64_t timingLogPacketsSent{0};
    std::uint64_t timingLogPointsSent{0};
};

} // namespace libera::lightspacenet
