#include "libera/liberaprotocol/LiberaProtocolController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/log/Log.hpp"
#include "libera/protocol/Codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>

namespace libera::liberaprotocol {
namespace {

namespace error_types = libera::core::error_types;
using namespace std::chrono_literals;

constexpr std::uint32_t defaultPointRate = 30000;
constexpr std::uint32_t defaultMaxPointRate = 100000;
constexpr std::uint32_t defaultMaxFramePoints = 300000;
constexpr std::uint32_t defaultMaxRecordPayloadBytes = 4u * 1024u * 1024u;
constexpr std::size_t rawPointBatchSize = 4096;
constexpr std::uint8_t maxLaserPointUserChannels = 2;
constexpr double scannerSyncUnitNanoseconds = 100000.0;
constexpr auto heartbeatInterval = 500ms;
constexpr auto heartbeatTimeout = 2000ms;

std::int16_t encodeSignedCoord(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    if (clamped <= -1.0f) {
        return std::numeric_limits<std::int16_t>::min();
    }
    return static_cast<std::int16_t>(std::llround(clamped * 32767.0f));
}

std::uint16_t encodeChannel(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::llround(clamped * 65535.0f));
}

std::chrono::steady_clock::duration pointPlaybackDuration(std::size_t pointCount,
                                                          std::uint32_t pointRate) {
    if (pointCount == 0 || pointRate == 0) {
        return std::chrono::steady_clock::duration::zero();
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(
            static_cast<double>(pointCount) / static_cast<double>(pointRate)));
}

std::uint32_t boundedOrDefault(std::uint32_t value, std::uint32_t fallback) {
    return value == 0 ? fallback : value;
}

} // namespace

LiberaProtocolController::LiberaProtocolController() = default;

LiberaProtocolController::LiberaProtocolController(LiberaProtocolControllerInfo info)
    : LiberaProtocolController() {
    latestInfo = std::move(info);
}

LiberaProtocolController::~LiberaProtocolController() {
    stopThread();
    close();
}

void LiberaProtocolController::updateDiscoveredInfo(const LiberaProtocolControllerInfo& info) {
    std::lock_guard<std::mutex> lock(latestInfoMutex);
    latestInfo = info;
}

libera::expected<void>
LiberaProtocolController::connect(const LiberaProtocolControllerInfo& info) {
    updateDiscoveredInfo(info);
    return connectToInfo(info);
}

libera::expected<void>
LiberaProtocolController::connectToInfo(const LiberaProtocolControllerInfo& info) {
    tcpClient = std::make_unique<net::TcpClient>();
    tcpClient->setConnectTimeout(1000ms);
    tcpClient->setDefaultTimeout(250ms);

    std::error_code ecAddr;
    auto address = net::asio::ip::make_address(info.address(), ecAddr);
    if (ecAddr) {
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    tcpEndpoint = net::tcp::endpoint(address, info.port());
    if (auto ec = tcpClient->connect(tcpEndpoint, 1000ms)) {
        logError("[LiberaProtocolController] TCP connect failed", info.address(), info.port(), ec.message());
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(ec);
    }
    tcpClient->setLowLatency();

    if (!performHandshake(info)) {
        close();
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(std::make_error_code(std::errc::protocol_error));
    }

    networkConnected.store(true, std::memory_order_relaxed);
    reconnectRequested.store(false, std::memory_order_relaxed);
    setConnectionState(true);
    resetStartupBlank();
    resetShutdownBlank();
    nextFrameId = 1;
    currentPointIndex = 0;
    nextSendAt = {};
    nextHeartbeatAt = std::chrono::steady_clock::now();
    lastPongAt = std::chrono::steady_clock::now();
    heartbeatSent = false;
    setEstimatedBufferCapacity(static_cast<int>(std::min<std::uint32_t>(
        session.maxFramePointCount,
        static_cast<std::uint32_t>(std::numeric_limits<int>::max()))));
    updateEstimatedBufferSnapshotNow(0, getPointRate());

    logInfo("[LiberaProtocolController] connected",
            info.address(),
            info.port(),
            "mode",
            static_cast<int>(session.streamMode),
            "userChannels",
            static_cast<int>(session.userChannelCount),
            "maxFramePoints",
            session.maxFramePointCount);
    return {};
}

bool LiberaProtocolController::performHandshake(const LiberaProtocolControllerInfo& info) {
    const auto requestedMode = info.supportsStreamMode(protocol::StreamMode::FrameByCount)
        ? protocol::StreamMode::FrameByCount
        : protocol::StreamMode::RawPointStream;
    const auto requestedUserChannels = static_cast<std::uint8_t>(std::min<std::uint32_t>(
        maxLaserPointUserChannels,
        info.maxUserChannelCount()));

    protocol::Hello hello;
    hello.senderName = "Libera";
    hello.requestedStreamMode = requestedMode;
    hello.requestedUserChannelCount = requestedUserChannels;
    hello.defaultPointRate = std::clamp<std::uint32_t>(
        boundedOrDefault(getPointRate(), defaultPointRate),
        1u,
        boundedOrDefault(info.maxPointRate(), defaultMaxPointRate));

    sender = protocol::Sender(requestedUserChannels);
    if (!writeMessage(sender.makeHello(hello), 500ms)) {
        return false;
    }

    protocol::Record record;
    if (!readRecord(record, 1000ms)) {
        return false;
    }
    if (record.type == protocol::RecordType::Reject) {
        protocol::Reject reject;
        std::string error;
        if (protocol::decodeReject(record.payload.data(), record.payload.size(), reject, error)) {
            logError("[LiberaProtocolController] session rejected",
                     static_cast<int>(reject.code),
                     reject.message);
        }
        return false;
    }
    if (record.type != protocol::RecordType::Accept) {
        return false;
    }

    protocol::Accept accept;
    std::string error;
    if (!protocol::decodeAccept(record.payload.data(), record.payload.size(), accept, error)) {
        logError("[LiberaProtocolController] ACCEPT decode failed", error);
        return false;
    }

    session.streamMode = accept.acceptedStreamMode;
    session.userChannelCount = accept.acceptedUserChannelCount;
    session.pointRate = boundedOrDefault(accept.defaultPointRate, hello.defaultPointRate);
    session.maxPointRate = boundedOrDefault(accept.maxPointRate,
                                            boundedOrDefault(info.maxPointRate(), defaultMaxPointRate));
    session.maxFramePointCount = boundedOrDefault(accept.maxFramePointCount,
                                                  boundedOrDefault(info.maxFramePointCount(),
                                                                   defaultMaxFramePoints));
    session.maxRecordPayloadSize = boundedOrDefault(accept.maxRecordPayloadSize,
                                                    defaultMaxRecordPayloadBytes);
    session.sessionId = accept.sessionId;
    session.featureFlags = accept.featureFlags;
    session.sessionStartedAt = std::chrono::steady_clock::now();
    scannerSyncSent = false;
    nextHeartbeatAt = {};
    lastPongAt = {};
    heartbeatSent = false;
    sender.setUserChannelCount(session.userChannelCount);
    LaserControllerStreaming::setPointRate(std::clamp<std::uint32_t>(
        session.pointRate,
        1u,
        session.maxPointRate));

    if (!writeMessage(sender.makeReady(), 500ms)) {
        return false;
    }

    protocol::StreamConfig config;
    config.defaultPointRate = getPointRate();
    config.streamMode = session.streamMode;
    config.userChannelCount = session.userChannelCount;
    if (!writeMessage(sender.makeStreamConfig(config), 500ms)) {
        return false;
    }

    return syncScannerSyncIfNeeded();
}

bool LiberaProtocolController::reconnectToLatestInfo() {
    std::optional<LiberaProtocolControllerInfo> info;
    {
        std::lock_guard<std::mutex> lock(latestInfoMutex);
        info = latestInfo;
    }
    if (!info) {
        return false;
    }
    auto result = connectToInfo(*info);
    if (!result) {
        logError("[LiberaProtocolController] reconnect failed", result.error().message());
        return false;
    }
    return true;
}

void LiberaProtocolController::close() {
    if (tcpClient) {
        tcpClient->close();
        tcpClient.reset();
    }
    networkConnected.store(false, std::memory_order_relaxed);
    reconnectRequested.store(false, std::memory_order_relaxed);
    scannerSyncSent = false;
    nextHeartbeatAt = {};
    lastPongAt = {};
    heartbeatSent = false;
    setScannerSyncPostProcessSuppressed(false);
    setConnectionState(false);
    clearEstimatedBufferState();
}

void LiberaProtocolController::markDisconnected() {
    networkConnected.store(false, std::memory_order_relaxed);
    reconnectRequested.store(true, std::memory_order_relaxed);
    scannerSyncSent = false;
    nextHeartbeatAt = {};
    lastPongAt = {};
    heartbeatSent = false;
    setScannerSyncPostProcessSuppressed(false);
    setConnectionState(false);
    recordConnectionError(error_types::network::connectionLost);
    if (tcpClient) {
        tcpClient->close();
    }
}

bool LiberaProtocolController::readRecord(protocol::Record& record,
                                          std::chrono::milliseconds timeout) {
    if (!tcpClient || !tcpClient->is_connected()) {
        return false;
    }

    std::array<std::uint8_t, protocol::RECORD_HEADER_SIZE> headerBytes{};
    if (auto ec = tcpClient->read_exact(headerBytes.data(), headerBytes.size(), timeout)) {
        return false;
    }

    protocol::RecordHeader header;
    std::string error;
    if (!protocol::decodeRecordHeader(headerBytes.data(), headerBytes.size(), header, error)) {
        logError("[LiberaProtocolController] record header decode failed", error);
        return false;
    }
    if (header.payloadSize > session.maxRecordPayloadSize) {
        logError("[LiberaProtocolController] refusing oversized record", header.payloadSize);
        return false;
    }

    std::vector<std::uint8_t> payload(header.payloadSize);
    if (!payload.empty()) {
        if (auto ec = tcpClient->read_exact(payload.data(), payload.size(), timeout)) {
            return false;
        }
    }

    record.type = header.type;
    record.flags = header.flags;
    record.sequence = header.sequence;
    record.payload = std::move(payload);
    return true;
}

bool LiberaProtocolController::writeMessage(const std::vector<std::uint8_t>& bytes,
                                            std::chrono::milliseconds timeout) {
    if (!tcpClient || !tcpClient->is_connected() || bytes.empty()) {
        return false;
    }
    if (auto ec = tcpClient->write_all(bytes.data(), bytes.size(), timeout)) {
        logError("[LiberaProtocolController] send failed", ec.message());
        markDisconnected();
        return false;
    }
    return true;
}

bool LiberaProtocolController::drainInboundRecords() {
    if (!tcpClient || !tcpClient->is_connected()) {
        return false;
    }

    while (networkConnected.load(std::memory_order_relaxed)) {
        std::error_code ec;
        const auto available = tcpClient->getSocket().available(ec);
        if (ec) {
            logError("[LiberaProtocolController] receive check failed", ec.message());
            markDisconnected();
            return false;
        }
        if (available < protocol::RECORD_HEADER_SIZE) {
            return true;
        }

        protocol::Record record;
        if (!readRecord(record, 500ms)) {
            logError("[LiberaProtocolController] inbound record failed");
            markDisconnected();
            return false;
        }

        if (record.type == protocol::RecordType::Pong && record.payload.size() == 8) {
            lastPongAt = std::chrono::steady_clock::now();
            continue;
        }
        if (record.type == protocol::RecordType::Close) {
            logInfo("[LiberaProtocolController] receiver closed session");
            markDisconnected();
            return false;
        }
        if (record.type == protocol::RecordType::Error) {
            logError("[LiberaProtocolController] receiver reported protocol error");
        }
    }
    return false;
}

bool LiberaProtocolController::serviceHeartbeat() {
    if (!networkConnected.load(std::memory_order_relaxed) || !drainInboundRecords()) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (heartbeatSent && lastPongAt != std::chrono::steady_clock::time_point{} &&
        (now - lastPongAt) >= heartbeatTimeout) {
        logError("[LiberaProtocolController] heartbeat timeout");
        markDisconnected();
        return false;
    }
    if (nextHeartbeatAt != std::chrono::steady_clock::time_point{} &&
        now < nextHeartbeatAt) {
        return true;
    }

    const auto timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
    if (!writeMessage(sender.makePing(timestamp), 250ms)) {
        return false;
    }
    heartbeatSent = true;
    nextHeartbeatAt = now + heartbeatInterval;
    return true;
}

bool LiberaProtocolController::receiverHandlesScannerSync() const noexcept {
    return (session.featureFlags & protocol::FeatureScannerSync) != 0;
}

std::int64_t LiberaProtocolController::currentScannerSyncOffsetNs() const {
    const double offsetUnits = getScannerSync();
    if (!std::isfinite(offsetUnits) || offsetUnits <= 0.0) {
        return 0;
    }
    const double maxNs =
        static_cast<double>(std::numeric_limits<std::int64_t>::max());
    const double clampedUnits = std::min(offsetUnits, maxNs / scannerSyncUnitNanoseconds);
    return static_cast<std::int64_t>(std::llround(clampedUnits * scannerSyncUnitNanoseconds));
}

bool LiberaProtocolController::syncScannerSyncIfNeeded() {
    if (!receiverHandlesScannerSync()) {
        scannerSyncSent = false;
        setScannerSyncPostProcessSuppressed(false);
        return true;
    }

    const auto offsetNs = currentScannerSyncOffsetNs();
    const bool enabled = isScannerSyncEnabled();
    if (scannerSyncSent &&
        offsetNs == lastScannerSyncOffsetNs &&
        enabled == lastScannerSyncEnabled) {
        return true;
    }

    protocol::ScannerSync scannerSync;
    scannerSync.offsetNs = offsetNs;
    scannerSync.enabled = enabled;
    if (!writeMessage(sender.makeScannerSync(scannerSync), 250ms)) {
        return false;
    }

    scannerSyncSent = true;
    lastScannerSyncOffsetNs = offsetNs;
    lastScannerSyncEnabled = enabled;
    return true;
}

void LiberaProtocolController::run() {
    resetStartupBlank();

    while (running.load()) {
        if (!networkConnected.load(std::memory_order_relaxed) ||
            !tcpClient ||
            !tcpClient->is_connected()) {
            setConnectionState(false);
            if (!reconnectToLatestInfo()) {
                std::this_thread::sleep_for(500ms);
                continue;
            }
        }

        if (!serviceHeartbeat()) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (nextSendAt != std::chrono::steady_clock::time_point{} && now < nextSendAt) {
            const auto sleepFor = std::min(
                5ms,
                std::chrono::duration_cast<std::chrono::milliseconds>(nextSendAt - now));
            std::this_thread::sleep_for(std::max(1ms, sleepFor));
            continue;
        }

        const bool sent = session.streamMode == protocol::StreamMode::FrameByCount
            ? sendFrameRecord()
            : sendRawPointsRecord();
        if (!sent) {
            std::this_thread::sleep_for(2ms);
        }
    }
}

void LiberaProtocolController::setPointRate(std::uint32_t pointRateValue) {
    const auto maxRate = session.maxPointRate > 0 ? session.maxPointRate : defaultMaxPointRate;
    const auto clamped = std::clamp<std::uint32_t>(pointRateValue, 1u, maxRate);
    LaserControllerStreaming::setPointRate(clamped);
}

bool LiberaProtocolController::sendFrameRecord() {
    if (!networkConnected.load(std::memory_order_relaxed)) {
        return false;
    }
    if (!syncScannerSyncIfNeeded()) {
        return false;
    }

    const auto activePointRate = getPointRate();
    const std::size_t maxFramePoints = std::max<std::size_t>(1, session.maxFramePointCount);
    core::Frame frame;
    FrameFillRequest request{};
    request.maximumPointsRequired = maxFramePoints;
    request.preferredPointCount = maxFramePoints;
    request.blankFramePointCount = std::min<std::size_t>(maxFramePoints, rawPointBatchSize);
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();
    request.currentPointIndex = currentPointIndex;
    request.advanceWhenAvailable = true;

    const bool suppressScannerSync = receiverHandlesScannerSync();
    if (suppressScannerSync) {
        setScannerSyncPostProcessSuppressed(true);
    }
    const bool requestOk = requestFrame(request, frame);
    if (suppressScannerSync) {
        setScannerSyncPostProcessSuppressed(false);
    }

    if (!requestOk || frame.points.empty()) {
        return false;
    }

    protocol::FrameMarker marker;
    marker.frameId = nextFrameId++;
    marker.pointRate = activePointRate;
    marker.framePointCount = static_cast<std::uint32_t>(std::min<std::size_t>(
        frame.points.size(),
        std::numeric_limits<std::uint32_t>::max()));
    if (frame.time != std::chrono::steady_clock::time_point{} &&
        frame.time > session.sessionStartedAt) {
        marker.targetBeginTimeNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                frame.time - session.sessionStartedAt).count());
    }

    if (!writeMessage(sender.makeFrameMarker(marker), 250ms)) {
        return false;
    }
    if (!sendPointsChunked(frame.points)) {
        return false;
    }

    const auto playback = pointPlaybackDuration(frame.points.size(), activePointRate);
    nextSendAt = std::chrono::steady_clock::now() + playback;
    currentPointIndex += frame.points.size();
    updateEstimatedBufferSnapshotNow(static_cast<int>(std::min<std::size_t>(
        frame.points.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                                     activePointRate);
    return true;
}

bool LiberaProtocolController::sendRawPointsRecord() {
    if (!syncScannerSyncIfNeeded()) {
        return false;
    }

    core::PointFillRequest request;
    request.minimumPointsRequired = rawPointBatchSize;
    request.maximumPointsRequired = rawPointBatchSize;
    request.estimatedFirstPointRenderTime = std::chrono::steady_clock::now();
    request.currentPointIndex = currentPointIndex;
    const bool suppressScannerSync = receiverHandlesScannerSync();
    if (suppressScannerSync) {
        setScannerSyncPostProcessSuppressed(true);
    }
    const bool requestOk = requestPoints(request);
    if (suppressScannerSync) {
        setScannerSyncPostProcessSuppressed(false);
    }

    if (!requestOk || pointsToSend.empty()) {
        return false;
    }

    const auto activePointRate = getPointRate();
    if (!sendPointsChunked(pointsToSend)) {
        return false;
    }
    const auto playback = pointPlaybackDuration(pointsToSend.size(), activePointRate);
    nextSendAt = std::chrono::steady_clock::now() + playback;
    currentPointIndex += pointsToSend.size();
    updateEstimatedBufferSnapshotNow(static_cast<int>(std::min<std::size_t>(
        pointsToSend.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                                     activePointRate);
    return true;
}

bool LiberaProtocolController::sendPointsChunked(const std::vector<core::LaserPoint>& points) {
    if (points.empty()) {
        return true;
    }

    const auto sampleSize = protocol::pointSampleSize(session.userChannelCount);
    const std::size_t pointsPerRecord = std::max<std::size_t>(
        1,
        session.maxRecordPayloadSize / std::max<std::size_t>(sampleSize, 1));

    std::size_t offset = 0;
    while (offset < points.size()) {
        const auto count = std::min<std::size_t>(pointsPerRecord, points.size() - offset);
        const auto samples = encodePoints(points, offset, count, session.userChannelCount);
        if (!writeMessage(sender.makePoints(samples), 250ms)) {
            return false;
        }
        offset += count;
    }
    return true;
}

std::vector<protocol::PointSample> LiberaProtocolController::encodePoints(
    const std::vector<core::LaserPoint>& points,
    std::size_t offset,
    std::size_t count,
    std::uint8_t userChannelCount) {
    std::vector<protocol::PointSample> samples;
    samples.reserve(count);
    const auto end = std::min(points.size(), offset + count);
    for (std::size_t i = offset; i < end; ++i) {
        const auto& point = points[i];
        protocol::PointSample sample;
        sample.x = encodeSignedCoord(point.x);
        sample.y = encodeSignedCoord(point.y);
        sample.r = encodeChannel(point.r);
        sample.g = encodeChannel(point.g);
        sample.b = encodeChannel(point.b);
        sample.i = encodeChannel(point.i);
        sample.user.resize(userChannelCount);
        if (userChannelCount > 0) {
            sample.user[0] = encodeChannel(point.u1);
        }
        if (userChannelCount > 1) {
            sample.user[1] = encodeChannel(point.u2);
        }
        samples.push_back(std::move(sample));
    }
    return samples;
}

} // namespace libera::liberaprotocol
