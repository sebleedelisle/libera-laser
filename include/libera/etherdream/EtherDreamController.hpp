#pragma once
#include "libera/core/Expected.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/net/NetConfig.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamCommand.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/etherdream/EtherDreamControllerInfo.hpp"
#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <string>
#include <optional>
#include <chrono>
#include <cstdint>

namespace libera::etherdream {

using libera::expected;
namespace ip = libera::net::asio::ip;

/**
 * @brief Streaming controller that talks to an EtherDream DAC.
 *
 * The controller inherits the worker thread lifecycle and point buffering from
 * `LaserControllerStreaming`. Streaming-specific timing (minimum refill sizes, sleep
 * cadence, etc.) is handled entirely within this class.
 *
 * Responsibilities:
 * - Maintain the TCP connection to the DAC.
 * - Poll status frames, decode them via `EtherDreamResponse`, and react.
 * - Request points from the user callback and stream controller-formatted frames.
 * - Drive the worker loop supplied by the base class.
 */
class EtherDreamController : public libera::core::LaserController {
public:
    EtherDreamController();
    explicit EtherDreamController(EtherDreamControllerInfo info);
    ~EtherDreamController();

    // non-copyable / non-movable
    EtherDreamController(const EtherDreamController&) = delete;
    EtherDreamController& operator=(const EtherDreamController&) = delete;
    EtherDreamController(EtherDreamController&&) = delete;
    EtherDreamController& operator=(EtherDreamController&&) = delete;

    struct Ack {
        EtherDreamStatus status{};
        char command = 0;
    };


    expected<void> connect();
    expected<void> connect(const EtherDreamControllerInfo& info);

    void close();                        // idempotent
    bool isConnected() const;           // const-safe
    /// Expose last network error (if any) for higher-level status reporting.
    std::optional<std::error_code> networkError() const { return lastNetworkError(); }
    /// Returns true when the TCP link is up and no network failure is recorded.
    bool hasActiveConnection() const;
    std::optional<core::BufferState> getBufferState() const override;
    void setPointRate(std::uint32_t pointRateValue) override;

    
protected:
    void run() override;

private:
    /// Push the desired point rate to the DAC only when playback is already
    /// running and the requested rate has changed. Initial/reconnect rate
    /// selection is carried by the begin command.
    void syncPointRate();


    /// Wait for the response frame to a specific command.
    expected<Ack>
    waitForResponse(char command,
                    bool allowWhileStopping = false,
                    std::uint64_t sequence = 0);

    /// Send the prepared command stored in commandBuffer and wait for its ACK.
    expected<Ack> sendCommand(bool allowWhileStopping = false);

    /// Issue the point-rate command ('q') and return the associated ACK.
    expected<Ack>
    sendPointRate(std::uint32_t rate);

    std::size_t calculateMinimumPoints();
    void sleepUntilNextPoints();

    void handleNetworkFailure(std::string_view where,
                              const std::error_code& ec);

    void resetPoints();
    void resetProtocolStateForConnection();
    void recordProtocolTx(std::uint64_t sequence, char opcode);
    void logProtocolRx(std::uint64_t sequence,
                       char expectedCommand,
                       const EtherDreamResponse& response,
                       const std::uint8_t* raw,
                       std::size_t rawSize) const;
    std::string describeProtocolTx(std::uint64_t sequence) const;

    int estimateBufferFullness() const;
    std::uint32_t maxSafePointRate() const;
    bool statusPointRateIsImplausible(const EtherDreamStatus& status) const;

    void updatePlaybackRequirements(const EtherDreamStatus& status);
    void applyFreshConnectionStatus(const EtherDreamStatus& status);
    core::PointFillRequest getFillRequest();
    bool shouldRequestPoints(const core::PointFillRequest& request) const;
    bool canSendData() const;
    void sendPoints();
    void sendStop(bool allowWhileStopping = false);
    void sendClear();
    void sendPrepare();
    void sendBegin();
    expected<Ack> sendPing();
    void pollStatus();
    void sendOrderlyStopBeforeClose();

    bool ensureConnected();
    bool performHandshake();

    int getBufferSize() const;

    std::optional<std::error_code> lastNetworkError() const;
    void clearNetworkError();
    EtherDreamCommand commandBuffer;

    struct ProtocolTxSnapshot {
        bool valid = false;
        std::chrono::steady_clock::time_point timestamp{};
        std::uint64_t sequence = 0;
        char opcode = 0;
        std::size_t bytes = 0;
        std::uint16_t pointCount = 0;
        std::uint16_t firstControl = 0;
        bool rateChangeBit = false;
        std::uint16_t beginFlags = 0;
        std::uint32_t commandRate = 0;
        std::size_t pendingRateChangeCount = 0;
        std::uint32_t localRate = 0;
        std::uint32_t lastSentRate = 0;
        EtherDreamStatus status{};
        std::string hex;
    };

    static constexpr std::size_t PROTOCOL_TX_HISTORY_SIZE = 8;
    const ProtocolTxSnapshot* findProtocolTxSnapshot(std::uint64_t sequence) const;

    EtherDreamStatus lastKnownStatus{};
    std::chrono::steady_clock::time_point lastReceiveTime{};
    libera::net::TcpClient tcpClient;
    std::optional<EtherDreamControllerInfo> controllerInfo;

    bool clearRequired = false;
    bool stopRequired = false;
    bool prepareRequired = false;
    bool beginRequired = false;
    bool connectionActive = false;

    std::optional<std::error_code> lastError;

    size_t pendingRateChangeCount = 0;

    // Tracks the rate we've successfully told the DAC about. Only touched from
    // the worker thread, so it doesn't need to be atomic. Reset to 0 on each
    // connection because the DAC's own state is unknown.
    std::uint32_t lastSentPointRate = 0;
    std::uint64_t connectionGeneration = 0;
    std::uint64_t nextCommandSequence = 0;
    std::array<ProtocolTxSnapshot, PROTOCOL_TX_HISTORY_SIZE> protocolTxHistory{};
    std::size_t nextProtocolTxHistoryIndex = 0;

    mutable std::atomic<int> lastEstimatedBufferFullness{0};
    mutable std::atomic<int> lastKnownBufferCapacity{0};
};

} // namespace libera::etherdream
