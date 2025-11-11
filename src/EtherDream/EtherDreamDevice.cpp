/**
 * @brief Implements the EtherDream DAC worker loop: connection, polling, and streaming.
 */
#include "libera/etherdream/EtherDreamDevice.hpp"

#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <string_view>
#include <system_error>
#include <thread>
#include <cstddef>  // for std::byte, std::to_integer
#include <cstdint>
#include <cctype>

using namespace std::chrono_literals; // Enable 100ms / 1s literals.

namespace libera::etherdream {

using libera::expected;
using libera::unexpected;
using DacAck = EtherDreamDevice::DacAck;
namespace ip = libera::net::asio::ip;
namespace asio = libera::net::asio;

EtherDreamDevice::EtherDreamDevice() {
    //setPointRate(config::ETHERDREAM_TARGET_POINT_RATE);
}

EtherDreamDevice::EtherDreamDevice(EtherDreamDeviceInfo info)
: deviceInfo(std::move(info)) {
    //setPointRate(config::ETHERDREAM_TARGET_POINT_RATE);
}

EtherDreamDevice::~EtherDreamDevice() {
    // Orderly shutdown: stop the worker thread and close the TCP connection.
    stop();
    close();
}




void EtherDreamDevice::run() {

    networkFailureEncountered = false;

    if (!tcpClient.is_open()) {
        logError("[EtherDreamDevice] run() called without an active connection.\n");
        running = false;
        return;
    }

    auto initialAck = waitForResponse('?');
    if (!initialAck) {
        if (auto pingAck = sendPing(); !pingAck) {
            handleNetworkFailure("initial ping", pingAck.error());
            return;
        }
    }
 
    while (running) {
        if (clearRequired) {
            sendClear();
        }

        if (prepareRequired) {
            sendPrepare();
        }

        sleepUntilNextPoints();

        auto req = getFillRequest();
        if (req.needsPoints(config::ETHERDREAM_MIN_PACKET_POINTS)) {
            requestPoints(req);
            sendPoints();
        }

        if (beginRequired) {
            sendBegin();
        }
    }

   
    close();
}

expected<DacAck>
EtherDreamDevice::waitForResponse(char command) {
    if (!running) {
        return unexpected(std::make_error_code(std::errc::operation_canceled));
    }
    if (!tcpClient.is_open()) {
        return unexpected(make_error_code(std::errc::not_connected));
    }

    std::array<std::uint8_t, 22> raw{};

    while (true) {
        std::size_t bytesTransferred = 0;
        if (auto ec = tcpClient.read_exact(raw.data(), raw.size(), &bytesTransferred); ec) {
            if (ec == asio::error::timed_out) {
                logError("[EtherDream] RX timeout waiting for command '", command, "'\n");
            }
            logError("[EtherDream] RX error ", ec.value(), ' ', ec.category().name(), " - ",
                      ec.message(), '\n');
            return unexpected(std::error_code(ec.value(), ec.category()));
        }

        EtherDreamResponse response;
        if (!response.decode(raw.data(), raw.size())) {
            logError("[EtherDreamDevice] Failed to decode ACK for command '", command, "'\n");
            return unexpected(make_error_code(std::errc::protocol_error));
        }

        const bool ackMatched = (response.response == 'a') &&
                                (static_cast<char>(response.command) == command);

        updatePlaybackRequirements(response.status, ackMatched);

        logInfo("[EtherDream] RX '", static_cast<char>(response.response),
                 "' for '", command, "' | ", response.status.describe(), '\n',
                 "           hex: ", EtherDreamStatus::toHexLine(raw.data(), raw.size()), '\n');

        if (response.response == 'I') {
            continue; // status frame only; wait for actual ACK
        }

        if (!ackMatched) {
            logError("[EtherDream] unexpected ACK: expected 'a' for '", command,
                      "' but got '", static_cast<char>(response.response),
                      "' for '", static_cast<char>(response.command), "'\n",
                      "           hex: ", EtherDreamStatus::toHexLine(raw.data(), raw.size()), '\n');
            return unexpected(make_error_code(std::errc::protocol_error));
        }

        return DacAck{response.status, static_cast<char>(response.command)};
    }
}
expected<void>
EtherDreamDevice::connect(const EtherDreamDeviceInfo& info) {
    deviceInfo = info;
    return connect();
}

expected<void>
EtherDreamDevice::connect() {
    if (!deviceInfo) {
        return unexpected(make_error_code(std::errc::invalid_argument));
    }

    std::error_code ec;
    auto ip = libera::net::asio::ip::make_address(deviceInfo->ip(), ec);
    if (ec) {
        logError("Invalid IP: ", ec.message(), "\n");
        return unexpected(ec);
    }

    libera::net::tcp::endpoint endpoint(ip, deviceInfo->port());

    if (auto connectError = tcpClient.connect(endpoint); connectError) {
        logError("[EtherDreamDevice] connect failed: ", connectError.message(),
                 " (to ", deviceInfo->ip(), ":", deviceInfo->port(), ")",
                 " timeout=", tcpClient.defaultTimeout().count(), "ms\n");
        return unexpected(connectError);
    }

    tcpClient.setLowLatency();
    tcpClient.setDefaultTimeout(1s);
    tcpClient.setConnectTimeout(3s);


    logInfo("[EtherDreamDevice] connected to ",
            deviceInfo->ip(), ":", deviceInfo->port(), "\n");

    return {};
}


void EtherDreamDevice::close() {
    logInfo("[EtherDreamDevice] close()\n");
    // Keep the operation idempotent so repeated calls are harmless.
    if (!tcpClient.is_open()) {
        return;
    }
    // Future improvement: cancel outstanding async operations before closing.
    tcpClient.close();
    clearNetworkError();
}

bool EtherDreamDevice::isConnected() const {
    return tcpClient.is_open();
}

void EtherDreamDevice::setPointRate(std::uint32_t pointRateValue) {
    LaserDeviceBase::setPointRate(pointRateValue);
}



expected<DacAck> EtherDreamDevice::sendCommand() {

    if (!running) {
        return unexpected(std::make_error_code(std::errc::operation_canceled));
    }

    if (!commandBuffer.isReady()) {
        return unexpected(make_error_code(std::errc::invalid_argument));
    }

    const char opcode = commandBuffer.commandOpcode();

    if (auto ec = tcpClient.write_all(commandBuffer.data(), commandBuffer.size()); ec) {
        commandBuffer.reset();
        return unexpected(ec);
    }

    auto ack = waitForResponse(opcode);
    commandBuffer.reset();
    return ack;
}

expected<DacAck> EtherDreamDevice::sendPointRate(std::uint16_t rate) {

    commandBuffer.setPointRateCommand(static_cast<std::uint32_t>(rate));

    logError("[EtherDream] TX 'q' (rate=", rate,
              ", timeout ", tcpClient.defaultTimeout().count(), "ms)\n");

    auto ack = sendCommand();
    if (!ack) {
        if (ack.error() == asio::error::timed_out) {
            logError("[EtherDream] point-rate command timed out after ", tcpClient.defaultTimeout().count(), "ms\n");
        }
        return ack;
    }

    rateChangePending = true;
    return ack;
}

std::size_t EtherDreamDevice::calculateMinimumPoints() {


    const int bufferFullness = estimateBufferFullness();

    int minPoints = millisToPoints(config::ETHERDREAM_MIN_BUFFER_MS); 
    if(bufferFullness>=minPoints) return 0; 
    else return static_cast<std::size_t>(minPoints - bufferFullness);
    
}


void EtherDreamDevice::handleNetworkFailure(std::string_view where,
                                     const std::error_code& ec) {
    if (!running.load(std::memory_order_relaxed)) {
        return; // Already stopping or stopped; no need to signal another failure.
    }

    logError("[EtherDreamDevice] ", where, " failed: ", ec.message(), "\n");
    running = false;
    networkFailureEncountered = true;
    lastError = ec;
}


void EtherDreamDevice::updatePlaybackRequirements(const EtherDreamStatus& status, bool commandAcked) {
    lastKnownStatus = status;
    lastReceiveTime = std::chrono::steady_clock::now();
    

    const bool estop = status.lightEngineState == LightEngineState::Estop;
    const bool underflow = (status.playbackFlags & 0x04u) != 0;
    clearRequired = estop || underflow || !commandAcked;

    prepareRequired = !clearRequired
        && status.lightEngineState == LightEngineState::Ready
        && status.playbackState == PlaybackState::Idle;

    const std::size_t bufferFullness = static_cast<std::size_t>(status.bufferFullness);
    beginRequired = !clearRequired
        && status.playbackState == PlaybackState::Prepared
        && bufferFullness >= config::ETHERDREAM_MIN_PACKET_POINTS;
}

core::PointFillRequest EtherDreamDevice::getFillRequest() {

    const auto bufferFullness = estimateBufferFullness();

    const auto bufferCapacity = getBufferSize();
    const auto freeSpace = bufferCapacity > bufferFullness ? bufferCapacity - bufferFullness : 0;
    const auto minimumPointsRequired =
    std::min<std::size_t>(calculateMinimumPoints(), freeSpace);
    
    core::PointFillRequest req;
    req.maximumPointsRequired = freeSpace;
    req.minimumPointsRequired = minimumPointsRequired;
    // ugh gross : 
    const auto bufferLead = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::milli>(pointsToMillis(bufferFullness)));

    req.estimatedFirstPointRenderTime =
        std::chrono::steady_clock::now() + bufferLead; 

    logInfo("[EtherDreamDevice Point fill request ", bufferFullness, ',',bufferCapacity,',', req.minimumPointsRequired,
             ' ', req.maximumPointsRequired, "\n");

    pointsToSend.clear();
    return req;
}

void EtherDreamDevice::sendPoints() {
    if (clearRequired || prepareRequired) {
        resetPoints();
        return;
    }

    if (pointsToSend.size() == 0) {
        return;
    }

    const bool injectRateChange = rateChangePending;
    const std::uint16_t pointCount =  static_cast<std::uint16_t>(pointsToSend.size());

    commandBuffer.setDataCommand(pointCount);

    for (std::size_t idx = 0; idx < pointCount; ++idx) {
        const bool setRateBit = injectRateChange && idx == 0;
        commandBuffer.addPoint(pointsToSend[idx], setRateBit);
    }

    if (commandBuffer.size() == 0) {
        handleNetworkFailure("packet serialization", std::make_error_code(std::errc::invalid_argument));
        resetPoints();
        return;
    }

    logInfo("[EtherDream] TX data: points=", pointsToSend.size(),
              " bytes=", commandBuffer.size(), "\n");

    auto dataAck = sendCommand();
    if (!dataAck) {
        handleNetworkFailure("data command", dataAck.error());
        resetPoints();
        return;
    }

    if (injectRateChange) {
        rateChangePending = false;
    }

    resetPoints();
}

void EtherDreamDevice::sendClear() {
    logInfo("[EtherDream] clear required -> send 'c'\n");
    commandBuffer.setSingleByteCommand('c');
    if (auto ack = sendCommand(); !ack) {
        handleNetworkFailure("clear command", ack.error());
    }
}

void EtherDreamDevice::sendPrepare() {
    logError("[EtherDream] prepare required -> send 'p'\n");
    commandBuffer.setSingleByteCommand('p');
    if (auto ack = sendCommand(); !ack) {
        handleNetworkFailure("prepare command", ack.error());
    }
}

void EtherDreamDevice::sendBegin() {
    logError("[EtherDream] begin required -> send 'b'\n");
    const auto targetRate = getPointRate();
    logInfo("[EtherDream] TX 'b' (rate=", targetRate,
            ", timeout ", tcpClient.defaultTimeout().count(), "ms)\n");
    commandBuffer.setBeginCommand(targetRate);
    if (auto ack = sendCommand(); !ack) {
        if (ack.error() == asio::error::timed_out) {
            logError("[EtherDream] begin write timeout after ", tcpClient.defaultTimeout().count(), "ms\n");
        }
        handleNetworkFailure("begin command", ack.error());
    }
}

expected<DacAck> EtherDreamDevice::sendPing() {
    commandBuffer.setSingleByteCommand('?');
    return sendCommand();
}

int EtherDreamDevice::estimateBufferFullness() const {
    const auto rate = lastKnownStatus.pointRate;
    if (rate == 0) {
        return lastKnownStatus.bufferFullness;
    }

    if (lastReceiveTime == std::chrono::steady_clock::time_point{}) {
        return lastKnownStatus.bufferFullness;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - lastReceiveTime;
    if (elapsed <= std::chrono::steady_clock::duration::zero()) {
        return lastKnownStatus.bufferFullness;
    }

    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (elapsedUs <= 0) {
        return lastKnownStatus.bufferFullness;
    }

    const double consumed =
        (static_cast<double>(rate) * static_cast<double>(elapsedUs)) / 1'000'000.0;
    const double estimated =
        static_cast<double>(lastKnownStatus.bufferFullness) - consumed;
    const double clamped =
        std::clamp(estimated, 0.0, static_cast<double>(getBufferSize()));

    return static_cast<std::uint16_t>(std::llround(clamped));
}

void EtherDreamDevice::ensureTargetPointRate() {
    if (clearRequired || prepareRequired || beginRequired) {
        return;
    }

    const auto targetRate = getPointRate();

    if (lastKnownStatus.playbackState == PlaybackState::Playing &&
        lastKnownStatus.pointRate != targetRate) {
        if (auto rateAck = sendPointRate(static_cast<std::uint16_t>(targetRate)); !rateAck) {
            handleNetworkFailure("point rate command", rateAck.error());
        }
    }
}

void EtherDreamDevice::sleepUntilNextPoints() {

    // strategy is : 
    // wait until the buffer has the minimum points in a packet available
    // if it already does then wait the minimum time between requests. 

    // we need to know the min allowable buffer level
    // we calculate this using the point rate as it needs to be 
    // time based rather than buffer size based

    int minPointsInBuffer = millisToPoints(config::ETHERDREAM_MIN_BUFFER_MS);
    const int bufferSize = getBufferSize();
    const int minPacketPoints = static_cast<int>(config::ETHERDREAM_MIN_PACKET_POINTS);

    if ((bufferSize - minPointsInBuffer) < minPacketPoints) {
        minPointsInBuffer = bufferSize - minPacketPoints;
    }

    // Estimate how long until the buffer drains to that minimum.
    const int fullness = static_cast<int>(estimateBufferFullness());
    const int pointsAboveMinimum = std::max(0, fullness - minPointsInBuffer);

    int millisToWait = static_cast<int>(std::llround(
        pointsToMillis(static_cast<std::size_t>(pointsAboveMinimum))));

    const int minSleep = static_cast<int>(config::ETHERDREAM_MIN_SLEEP.count());
    const int maxSleep = static_cast<int>(config::ETHERDREAM_MAX_SLEEP.count());

    if (millisToWait < minSleep) {
        millisToWait = minSleep;
    } else if (millisToWait > maxSleep) {
        millisToWait = maxSleep;
    }

    logInfo("[EtherDreamDevice] Sleeping for ", millisToWait, "\n");
    std::this_thread::sleep_for(std::chrono::milliseconds{millisToWait});
}

void EtherDreamDevice::resetPoints() {
    pointsToSend.clear();
}


int EtherDreamDevice::getBufferSize() const {
    if (deviceInfo) {
        const int size = deviceInfo->bufferSizeValue();
        if (size > 0) {
            return size;
        }
    }
    return static_cast<int>(0);
}

std::optional<std::error_code> EtherDreamDevice::lastNetworkError() const {
    return lastError;
}

void EtherDreamDevice::clearNetworkError() {
    lastError.reset();
}

} // namespace libera::etherdream
