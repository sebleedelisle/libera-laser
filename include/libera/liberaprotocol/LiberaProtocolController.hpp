#pragma once

#include "libera/core/Expected.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/liberaprotocol/LiberaProtocolControllerInfo.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/protocol/Sender.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace libera::liberaprotocol {

class LiberaProtocolController : public core::LaserController {
public:
    LiberaProtocolController();
    explicit LiberaProtocolController(LiberaProtocolControllerInfo info);
    ~LiberaProtocolController() override;

    libera::expected<void> connect(const LiberaProtocolControllerInfo& info);
    void close();
    void updateDiscoveredInfo(const LiberaProtocolControllerInfo& info);

protected:
    void run() override;
    void setPointRate(std::uint32_t pointRate) override;

private:
    struct NegotiatedSession {
        protocol::StreamMode streamMode = protocol::StreamMode::RawPointStream;
        std::uint8_t userChannelCount = 0;
        std::uint32_t pointRate = 30000;
        std::uint32_t maxPointRate = 100000;
        std::uint32_t maxFramePointCount = 300000;
        std::uint32_t maxRecordPayloadSize = 4u * 1024u * 1024u;
        std::uint64_t sessionId = 0;
        std::uint32_t featureFlags = 0;
        std::chrono::steady_clock::time_point sessionStartedAt{};
    };

    libera::expected<void> connectToInfo(const LiberaProtocolControllerInfo& info);
    bool reconnectToLatestInfo();
    bool performHandshake(const LiberaProtocolControllerInfo& info);
    bool readRecord(protocol::Record& record, std::chrono::milliseconds timeout);
    bool writeMessage(const std::vector<std::uint8_t>& bytes,
                      std::chrono::milliseconds timeout);
    bool serviceHeartbeat();
    bool drainInboundRecords();
    bool receiverHandlesScannerSync() const noexcept;
    bool syncScannerSyncIfNeeded();
    bool sendFrameRecord();
    bool sendRawPointsRecord();
    bool sendPointsChunked(const std::vector<core::LaserPoint>& points);
    void markDisconnected();
    std::int64_t currentScannerSyncOffsetNs() const;

    static std::vector<protocol::PointSample> encodePoints(
        const std::vector<core::LaserPoint>& points,
        std::size_t offset,
        std::size_t count,
        std::uint8_t userChannelCount);

    std::unique_ptr<net::TcpClient> tcpClient;
    net::tcp::endpoint tcpEndpoint;
    protocol::Sender sender;
    NegotiatedSession session;

    std::atomic<bool> networkConnected{false};
    std::atomic<bool> reconnectRequested{false};

    mutable std::mutex latestInfoMutex;
    std::optional<LiberaProtocolControllerInfo> latestInfo;

    std::uint64_t nextFrameId = 1;
    std::uint64_t currentPointIndex = 0;
    std::chrono::steady_clock::time_point nextSendAt{};
    std::chrono::steady_clock::time_point nextHeartbeatAt{};
    std::chrono::steady_clock::time_point lastPongAt{};
    bool heartbeatSent = false;
    bool scannerSyncSent = false;
    std::int64_t lastScannerSyncOffsetNs = 0;
    bool lastScannerSyncEnabled = false;
};

} // namespace libera::liberaprotocol
