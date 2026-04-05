#pragma once
#include "libera/core/Expected.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/net/NetConfig.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamCommand.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/etherdream/EtherDreamControllerInfo.hpp"
#include <memory>
#include <string_view>
#include <optional>
#include <chrono>

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

    
protected:
    void run() override;
    bool sendPointRateToDevice(std::uint32_t rate) override;

private:
    /// Wait for the response frame to a specific command.
    expected<Ack>
    waitForResponse(char command);

    /// Send the prepared command stored in commandBuffer and wait for its ACK.
    expected<Ack>
    sendCommand();

    /// Issue the point-rate command ('q') and return the associated ACK.
    expected<Ack>
    sendPointRate(std::uint32_t rate);

    std::size_t calculateMinimumPoints();
    void sleepUntilNextPoints();

    void handleNetworkFailure(std::string_view where,
                              const std::error_code& ec);

    void resetPoints();

    int estimateBufferFullness() const;

    void updatePlaybackRequirements(const EtherDreamStatus& status, bool commandAcked);
    core::PointFillRequest getFillRequest();
    void sendPoints();
    void sendClear();
    void sendPrepare();
    void sendBegin();
    expected<Ack> sendPing();

    bool ensureConnected();
    bool performHandshake();

    int getBufferSize() const;

    std::optional<std::error_code> lastNetworkError() const;
    void clearNetworkError();
    EtherDreamCommand commandBuffer;

    EtherDreamStatus lastKnownStatus{};
    std::chrono::steady_clock::time_point lastReceiveTime{};
    libera::net::TcpClient tcpClient;
    std::optional<EtherDreamControllerInfo> controllerInfo;

    bool clearRequired = false;
    bool prepareRequired = false;
    bool beginRequired = false;
    bool connectionActive = false;

    std::optional<std::error_code> lastError;

    size_t pendingRateChangeCount = 0;

    mutable std::atomic<int> lastEstimatedBufferFullness{0};
    mutable std::atomic<int> lastKnownBufferCapacity{0};
};

} // namespace libera::etherdream
