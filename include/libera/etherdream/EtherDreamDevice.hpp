#pragma once
#include "libera/core/Expected.hpp"
#include "libera/core/LaserDevice.hpp"
#include "libera/net/NetConfig.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamCommand.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/etherdream/EtherDreamDeviceInfo.hpp"
#include <deque>
#include <memory>
#include <string_view>
#include <optional>
#include <chrono>
#include <mutex>

namespace libera::etherdream {

using libera::expected;
namespace ip = libera::net::asio::ip;

/**
 * @brief Streaming controller that talks to an EtherDream DAC.
 *
 * The device inherits the worker thread lifecycle and point buffering from
 * `LaserDeviceBase`. Streaming-specific timing (minimum refill sizes, sleep
 * cadence, etc.) is handled entirely within this class.
 *
 * Responsibilities:
 * - Maintain the TCP connection to the DAC.
 * - Poll status frames, decode them via `EtherDreamResponse`, and react.
 * - Request points from the user callback and stream device-formatted frames.
 * - Drive the worker loop supplied by the base class.
 */
class EtherDreamDevice : public libera::core::LaserDevice {
public:
    EtherDreamDevice();
    explicit EtherDreamDevice(EtherDreamDeviceInfo info);
    ~EtherDreamDevice();

    // non-copyable / non-movable
    EtherDreamDevice(const EtherDreamDevice&) = delete;
    EtherDreamDevice& operator=(const EtherDreamDevice&) = delete;
    EtherDreamDevice(EtherDreamDevice&&) = delete;
    EtherDreamDevice& operator=(EtherDreamDevice&&) = delete;

    struct DacAck {
        EtherDreamStatus status{};
        char command = 0;
    };


    expected<void> connect();
    expected<void> connect(const EtherDreamDeviceInfo& info);

    void setPointRate(std::uint32_t pointRate) override;

    void close();                        // idempotent
    bool isConnected() const;           // const-safe

    
protected:
    void run() override;


private:
    std::optional<std::uint16_t> nextPendingRateChange();

    /// Wait for the response frame to a specific command.
    expected<DacAck>
    waitForResponse(char command);

    /// Send the prepared command stored in commandBuffer_ and wait for its ACK.
    expected<DacAck>
    sendCommand();

    /// Issue the point-rate command ('q') and return the associated ACK.
    expected<DacAck>
    sendPointRate(std::uint16_t rate);

    std::size_t calculateMinimumPoints();

    long long p();
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
    expected<DacAck> sendPing();
    // void ensureTargetPointRate();

    bool ensureConnected();
    bool performHandshake();

    int getBufferSize() const;

    std::optional<std::error_code> lastNetworkError() const;
    void clearNetworkError();
    EtherDreamCommand commandBuffer;

    EtherDreamStatus lastKnownStatus{};
    std::chrono::steady_clock::time_point lastReceiveTime{};
    libera::net::TcpClient tcpClient;
    std::optional<EtherDreamDeviceInfo> deviceInfo;

    bool clearRequired = false;
    bool prepareRequired = false;
    bool beginRequired = false;
    bool connectionActive = false;

    bool networkFailureEncountered = false;
    std::optional<std::error_code> lastError;

    std::mutex pendingRatesMutex;
    std::deque<std::uint16_t> pendingRateChanges;
    size_t pendingRateChangeCount = 0; 
};

} // namespace libera::etherdream
