#include "libera/lightspacenet/LightSpaceNetController.hpp"

#include "libera/core/ControllerErrorTypes.hpp"
#include "libera/lightspacenet/LightSpaceNetConfig.hpp"
#include "libera/lightspacenet/LightSpaceNetPacket.hpp"
#include "libera/log/Log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace libera::lightspacenet {

namespace error_types = libera::core::error_types;

namespace {

constexpr auto reconnectRetryDelay = std::chrono::milliseconds(100);
constexpr auto commandRetryDelay = std::chrono::seconds(1);
constexpr std::array<std::uint8_t, 10> protocolHeader{
    'L', 'I', 'G', 'H', 'T', 'S', 'P', 'A', 'C', 'E'
};
constexpr std::size_t minimumPacketSize = 20;

bool ackPayloadMatchesCommand(const LightSpaceNetPacket& packet,
                              std::uint8_t commandWord) {
    if (packet.packetType != LightSpaceNetConfig::PACKET_TYPE_COMMAND ||
        packet.commandWord != LightSpaceNetConfig::CMD_COMMAND_ACK ||
        packet.payload.size() < 2) {
        return false;
    }

    const auto acknowledged = readBe16(packet.payload.data());
    // The document says the ACK carries a two-byte "command word", but does
    // not state whether the high byte is zero or a packet-type prefix. Accept
    // either shape so the first hardware test is not blocked by that ambiguity.
    return acknowledged == commandWord ||
           static_cast<std::uint8_t>(acknowledged & 0xFFu) == commandWord;
}

std::uint64_t steadyMillis() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string normalizedEnvironmentValue(const char* name) {
    const char* rawValue = std::getenv(name);
    if (!rawValue) {
        return {};
    }

    std::string value(rawValue);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool environmentFlagEnabled(const char* name) {
    const auto value = normalizedEnvironmentValue(name);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::chrono::microseconds nonNegativeMicros(std::chrono::microseconds value) {
    return std::chrono::microseconds(std::max<std::int64_t>(0, value.count()));
}

std::chrono::microseconds smoothWriteLead(std::chrono::microseconds previous,
                                          std::chrono::steady_clock::duration current) {
    const auto previousMicros = std::max<std::int64_t>(0, previous.count());
    const auto currentMicros = std::max<std::int64_t>(
        0,
        std::chrono::duration_cast<std::chrono::microseconds>(current).count());
    if (previousMicros == 0) {
        return std::chrono::microseconds(currentMicros);
    }
    return std::chrono::microseconds(((previousMicros * 3) + currentMicros) / 4);
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

std::vector<core::LaserPoint> makeBlankPatternPoints(std::size_t pointCount) {
    std::vector<core::LaserPoint> blankPoints(pointCount);
    for (auto& point : blankPoints) {
        point.i = 0.0f;
    }
    return blankPoints;
}

const char* coordinateEncodingName(LightSpaceNetCoordinateEncoding encoding) {
    if (encoding == LightSpaceNetCoordinateEncoding::Signed16) {
        return "signed16";
    }
    if (encoding == LightSpaceNetCoordinateEncoding::Unsigned16) {
        return "unsigned16";
    }
    if (encoding == LightSpaceNetCoordinateEncoding::Signed15) {
        return "signed15";
    }
    if (encoding == LightSpaceNetCoordinateEncoding::Unsigned15) {
        return "unsigned15";
    }
    if (encoding == LightSpaceNetCoordinateEncoding::Signed12) {
        return "signed12";
    }
    return "unsigned12";
}

const char* coordinateByteOrderName(LightSpaceNetCoordinateByteOrder byteOrder) {
    if (byteOrder == LightSpaceNetCoordinateByteOrder::LittleEndian) {
        return "little";
    }
    return "big";
}

LightSpaceNetCoordinateOptions coordinateOptionsFromEnvironment() {
    LightSpaceNetCoordinateOptions options;
    options.encoding = LightSpaceNetCoordinateEncoding::Unsigned12;
    options.scale = 1.0f;

    const auto coordinateMode = normalizedEnvironmentValue("LIBERA_LIGHTSPACENET_COORDS");
    if (coordinateMode == "unsigned" || coordinateMode == "unsigned16") {
        options.encoding = LightSpaceNetCoordinateEncoding::Unsigned16;
    } else if (coordinateMode == "signed15") {
        options.encoding = LightSpaceNetCoordinateEncoding::Signed15;
    } else if (coordinateMode == "unsigned15") {
        options.encoding = LightSpaceNetCoordinateEncoding::Unsigned15;
    } else if (coordinateMode == "signed12") {
        options.encoding = LightSpaceNetCoordinateEncoding::Signed12;
    } else if (coordinateMode == "unsigned12") {
        options.encoding = LightSpaceNetCoordinateEncoding::Unsigned12;
    } else if (coordinateMode == "signed" || coordinateMode == "signed16") {
        options.encoding = LightSpaceNetCoordinateEncoding::Signed16;
    } else if (coordinateMode.empty()) {
        options.encoding = LightSpaceNetCoordinateEncoding::Unsigned12;
    } else {
        logError("[LightSpaceNetController] unknown LIBERA_LIGHTSPACENET_COORDS",
                 coordinateMode,
                 "using unsigned12");
    }

    const auto byteOrder = normalizedEnvironmentValue("LIBERA_LIGHTSPACENET_COORD_ENDIAN");
    if (byteOrder == "little" || byteOrder == "le") {
        options.byteOrder = LightSpaceNetCoordinateByteOrder::LittleEndian;
    } else if (byteOrder == "big" || byteOrder == "be" || byteOrder.empty()) {
        options.byteOrder = LightSpaceNetCoordinateByteOrder::BigEndian;
    } else {
        logError("[LightSpaceNetController] unknown LIBERA_LIGHTSPACENET_COORD_ENDIAN",
                 byteOrder,
                 "using big");
    }

    options.invertX = environmentFlagEnabled("LIBERA_LIGHTSPACENET_INVERT_X");
    options.invertY = environmentFlagEnabled("LIBERA_LIGHTSPACENET_INVERT_Y");
    options.swapXY = environmentFlagEnabled("LIBERA_LIGHTSPACENET_SWAP_XY");

    if (const char* scaleValue = std::getenv("LIBERA_LIGHTSPACENET_SCALE");
        scaleValue && scaleValue[0] != '\0') {
        char* end = nullptr;
        const float parsed = std::strtof(scaleValue, &end);
        if (end != scaleValue && std::isfinite(parsed)) {
            options.scale = std::clamp(parsed, 0.0f, 4.0f);
        } else {
            logError("[LightSpaceNetController] invalid LIBERA_LIGHTSPACENET_SCALE",
                     scaleValue,
                     "using 1.0");
        }
    }

    if (const char* offsetValue = std::getenv("LIBERA_LIGHTSPACENET_OFFSET_X");
        offsetValue && offsetValue[0] != '\0') {
        char* end = nullptr;
        const float parsed = std::strtof(offsetValue, &end);
        if (end != offsetValue && std::isfinite(parsed)) {
            options.offsetX = std::clamp(parsed, -1.0f, 1.0f);
        }
    }

    if (const char* offsetValue = std::getenv("LIBERA_LIGHTSPACENET_OFFSET_Y");
        offsetValue && offsetValue[0] != '\0') {
        char* end = nullptr;
        const float parsed = std::strtof(offsetValue, &end);
        if (end != offsetValue && std::isfinite(parsed)) {
            options.offsetY = std::clamp(parsed, -1.0f, 1.0f);
        }
    }
    return options;
}

std::size_t patternPointLimitFromEnvironment() {
    const char* rawValue = std::getenv("LIBERA_LIGHTSPACENET_PATTERN_POINTS");
    if (!rawValue || rawValue[0] == '\0') {
        return LightSpaceNetConfig::DEFAULT_PATTERN_POINTS;
    }

    char* end = nullptr;
    const long parsed = std::strtol(rawValue, &end, 10);
    if (end == rawValue || parsed <= 0) {
        logError("[LightSpaceNetController] invalid LIBERA_LIGHTSPACENET_PATTERN_POINTS",
                 rawValue,
                 "using default");
        return LightSpaceNetConfig::DEFAULT_PATTERN_POINTS;
    }

    return static_cast<std::size_t>(std::clamp<long>(
        parsed,
        1,
        static_cast<long>(LightSpaceNetConfig::MAX_SOURCE_FRAME_POINTS)));
}

std::uint32_t pointRateFromEnvironment(std::uint32_t fallbackPointRate) {
    const char* rawValue = std::getenv("LIBERA_LIGHTSPACENET_POINT_RATE");
    if (!rawValue || rawValue[0] == '\0') {
        return LightSpaceNetConfig::clampPointRate(fallbackPointRate);
    }

    char* end = nullptr;
    const long parsed = std::strtol(rawValue, &end, 10);
    if (end == rawValue || parsed <= 0) {
        logError("[LightSpaceNetController] invalid LIBERA_LIGHTSPACENET_POINT_RATE",
                 rawValue,
                 "using",
                 fallbackPointRate);
        return LightSpaceNetConfig::clampPointRate(fallbackPointRate);
    }

    const auto clamped = std::clamp<long>(
        parsed,
        static_cast<long>(LightSpaceNetConfig::MIN_POINT_RATE),
        static_cast<long>(LightSpaceNetConfig::MAX_POINT_RATE));
    return static_cast<std::uint32_t>(clamped);
}

std::chrono::milliseconds patternUpdateIntervalFromEnvironment() {
    const char* rawValue = std::getenv("LIBERA_LIGHTSPACENET_PATTERN_MS");
    if (!rawValue || rawValue[0] == '\0') {
        return LightSpaceNetConfig::DEFAULT_PATTERN_UPDATE_INTERVAL;
    }

    char* end = nullptr;
    const long parsed = std::strtol(rawValue, &end, 10);
    if (end == rawValue || parsed <= 0) {
        logError("[LightSpaceNetController] invalid LIBERA_LIGHTSPACENET_PATTERN_MS",
                 rawValue,
                 "using default");
        return LightSpaceNetConfig::DEFAULT_PATTERN_UPDATE_INTERVAL;
    }

    return std::chrono::milliseconds(std::clamp<long>(parsed, 1, 1000));
}

} // namespace

LightSpaceNetController::LightSpaceNetController() = default;

LightSpaceNetController::LightSpaceNetController(LightSpaceNetControllerInfo info)
    : LightSpaceNetController() {
    ipAddress = info.ipAddress();
}

LightSpaceNetController::~LightSpaceNetController() {
    stopThread();
    close();
}

void LightSpaceNetController::updateDiscoveredStatus(const LightSpaceNetStatus& status) {
    {
        std::lock_guard<std::mutex> lock(latestStatusMutex);
        latestStatus = status;
    }

    if (ipAddress != status.ipAddress) {
        ipAddress = status.ipAddress;
        if (networkConnected.load(std::memory_order_relaxed)) {
            reconnectRequested.store(true, std::memory_order_relaxed);
        }
    }

    if (!networkConnected.load(std::memory_order_relaxed)) {
        reconnectRequested.store(true, std::memory_order_relaxed);
    }
}

libera::expected<void> LightSpaceNetController::connect(const LightSpaceNetControllerInfo& info) {
    updateDiscoveredStatus(info.status());
    return connectToStatus(info.status());
}

libera::expected<void> LightSpaceNetController::connectToStatus(const LightSpaceNetStatus& status) {
    ipAddress = status.ipAddress;
    LaserControllerStreaming::setPointRate(pointRateFromEnvironment(getPointRate()));
    if (tcpClient) {
        tcpClient->close();
        tcpClient.reset();
    }

    std::error_code ecAddr;
    auto address = net::asio::ip::make_address(ipAddress, ecAddr);
    if (ecAddr) {
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    tcpEndpoint = net::tcp::endpoint(address, LightSpaceNetConfig::NETWORK_PORT);

    // Hardware testing showed that LS-Net point packets behave as complete
    // pattern uploads over TCP. UDP remains discovery-only in this backend.
    tcpClient = std::make_unique<net::TcpClient>();
    tcpClient->setConnectTimeout(std::chrono::milliseconds(1000));
    tcpClient->setDefaultTimeout(std::chrono::milliseconds(200));
    if (auto ec = tcpClient->connect(tcpEndpoint, std::chrono::milliseconds(1000))) {
        logError("[LightSpaceNetController] TCP connect failed", ec.message());
        recordConnectionError(error_types::network::connectFailed);
        return libera::unexpected(ec);
    }
    tcpClient->setLowLatency();

    networkConnected.store(true, std::memory_order_relaxed);
    reconnectRequested.store(false, std::memory_order_relaxed);
    setConnectionState(true);

    const auto now = std::chrono::steady_clock::now();
    lastHeartbeatSentTime = {};
    lastHeartbeatReplyTime = now;
    lastSentPointRate = 0;
    pointRatePushNeeded = true;
    nextPointRateSyncTime = {};
    lastSentArmed = !isArmed();
    laserStatePushNeeded = true;
    nextLaserStateSyncTime = {};
    coordinateOptions = coordinateOptionsFromEnvironment();
    commandAckRequired = !environmentFlagEnabled("LIBERA_LIGHTSPACENET_SKIP_COMMAND_ACK");
    strictHeartbeat = environmentFlagEnabled("LIBERA_LIGHTSPACENET_REQUIRE_HEARTBEAT");
    timingLogEnabled = environmentFlagEnabled("LIBERA_LIGHTSPACENET_TIMING_LOG");
    heartbeatTimeoutLogged = false;
    patternUpdateInterval = patternUpdateIntervalFromEnvironment();
    lastPatternSentTime = {};
    nextPatternSendTime = {};
    lastIncomingPollTime = {};
    patternPointLimit = patternPointLimitFromEnvironment();
    lastSentPacketPointCount = 0;
    lastSentPacketBytes = 0;
    lastSubmittedPatternPoints = 0;
    estimatedWriteLead = std::chrono::microseconds(0);
    currentPointIndex = 0;
    clearFrameTransportSubmissionEstimate();
    tcpReceiveBuffer.clear();
    timingLogWindowStart = {};
    timingLogPacketsSent = 0;
    timingLogPointsSent = 0;

    logInfo("[LightSpaceNetController] coordinate mapping",
            coordinateEncodingName(coordinateOptions.encoding),
            "invertX", coordinateOptions.invertX ? "yes" : "no",
            "invertY", coordinateOptions.invertY ? "yes" : "no",
            "swapXY", coordinateOptions.swapXY ? "yes" : "no",
            "endian", coordinateByteOrderName(coordinateOptions.byteOrder),
            "scale", coordinateOptions.scale,
            "offsetX", coordinateOptions.offsetX,
            "offsetY", coordinateOptions.offsetY);
    logInfo("[LightSpaceNetController] transport tcp pattern");
    logInfo("[LightSpaceNetController] point rate",
            getPointRate(),
            "pps -> scan rate",
            static_cast<int>(LightSpaceNetConfig::scanFrequencyKilohertz(getPointRate())),
            "kHz");
    logInfo("[LightSpaceNetController] command ACK mode",
            commandAckRequired ? "required" : "fire-and-forget");
    logInfo("[LightSpaceNetController] timing log",
            timingLogEnabled ? "enabled" : "disabled");
    logInfo("[LightSpaceNetController] maximum frame points", patternPointLimit);
    logInfo("[LightSpaceNetController] heartbeat timeout",
            strictHeartbeat ? "disconnects" : "advisory");
    logInfo("[LightSpaceNetController] minimum pattern update interval",
            patternUpdateInterval.count(),
            "ms");

    setEstimatedBufferCapacity(static_cast<int>(patternPointLimit));
    updateEstimatedBufferSnapshotNow(0, getPointRate());
    resetStartupBlank();
    setVerbose(false);
    return {};
}

bool LightSpaceNetController::reconnectToLatestStatus() {
    std::optional<LightSpaceNetStatus> status;
    {
        std::lock_guard<std::mutex> lock(latestStatusMutex);
        status = latestStatus;
    }
    if (!status) {
        return false;
    }

    auto result = connectToStatus(*status);
    if (!result) {
        logError("[LightSpaceNetController] reconnect failed", result.error().message());
        return false;
    }
    return true;
}

void LightSpaceNetController::close() {
    if (networkConnected.load(std::memory_order_relaxed) && hasTcpConnection()) {
        // LS-Net devices can keep repeating the last uploaded pattern. Blank
        // that stored pattern first, then send laser-off as a best-effort
        // command without waiting for an ACK during teardown.
        sendBlankPatternForShutdown();
        (void)sendPacket(buildLaserSwitchPacket(false), std::chrono::milliseconds(50));
    }

    networkConnected.store(false, std::memory_order_relaxed);
    reconnectRequested.store(false, std::memory_order_relaxed);
    setConnectionState(false);
    clearEstimatedBufferState();
    if (tcpClient) {
        tcpClient->close();
    }
}

void LightSpaceNetController::run() {
    resetStartupBlank();

    while (running.load()) {
        if (!networkConnected.load(std::memory_order_relaxed) || !hasTcpConnection()) {
            setConnectionState(false);
            networkConnected.store(false, std::memory_order_relaxed);
            reconnectRequested.store(false, std::memory_order_relaxed);
            if (!reconnectToLatestStatus()) {
                reconnectRequested.store(true, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            std::this_thread::sleep_for(reconnectRetryDelay);
            continue;
        }

        setConnectionState(true);
        syncPointRate();
        syncLaserState();
        sendHeartbeatIfDue();

        (void)sendFramePattern();
        const auto now = std::chrono::steady_clock::now();
        if (lastIncomingPollTime == std::chrono::steady_clock::time_point{} ||
            now - lastIncomingPollTime >= LightSpaceNetConfig::INCOMING_POLL_INTERVAL) {
            pollIncomingPackets();
            lastIncomingPollTime = now;
        }

        const auto sleepStart = std::chrono::steady_clock::now();
        int sleepMillis = 1;
        if (nextPatternSendTime != std::chrono::steady_clock::time_point{} &&
            nextPatternSendTime > sleepStart) {
            const auto millisUntilNext = std::chrono::duration_cast<std::chrono::milliseconds>(
                nextPatternSendTime - sleepStart).count();
            sleepMillis = std::clamp<int>(
                static_cast<int>(millisUntilNext),
                1,
                5);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMillis));
    }
}

void LightSpaceNetController::setPointRate(std::uint32_t pointRateValue) {
    const auto clampedPointRate = LightSpaceNetConfig::clampPointRate(pointRateValue);
    const bool deviceRateNeedsUpdate =
        clampedPointRate != getPointRate() || lastSentPointRate != clampedPointRate;

    LaserControllerStreaming::setPointRate(clampedPointRate);
    if (!deviceRateNeedsUpdate) {
        return;
    }

    pointRatePushNeeded = true;
    nextPointRateSyncTime = {};
}

bool LightSpaceNetController::sendPacket(const std::vector<std::uint8_t>& packet,
                                         std::chrono::milliseconds timeout) {
    if (!hasTcpConnection() || packet.empty()) {
        recordConnectionError(error_types::network::sendFailed);
        networkConnected.store(false, std::memory_order_relaxed);
        reconnectRequested.store(true, std::memory_order_relaxed);
        return false;
    }

    std::error_code ec = tcpClient->write_all(packet.data(), packet.size(), timeout);
    if (ec) {
        logError("[LightSpaceNetController] send failed", ec.message());
        recordConnectionError(error_types::network::sendFailed);
        networkConnected.store(false, std::memory_order_relaxed);
        reconnectRequested.store(true, std::memory_order_relaxed);
        setConnectionState(false);
        return false;
    }
    return true;
}

bool LightSpaceNetController::sendControlCommand(
    std::uint8_t commandWord,
    const std::vector<std::uint8_t>& packet) {
    if (commandAckRequired) {
        return sendReliableCommand(commandWord, packet);
    }

    // This escape hatch is useful while validating firmware that accepts
    // commands but does not return documented ACK packets on the TCP session.
    return sendPacket(packet, std::chrono::milliseconds(50));
}

bool LightSpaceNetController::sendReliableCommand(
    std::uint8_t commandWord,
    const std::vector<std::uint8_t>& packet) {
    for (int attempt = 0; attempt < LightSpaceNetConfig::COMMAND_ACK_ATTEMPTS; ++attempt) {
        if (!sendPacket(packet, LightSpaceNetConfig::COMMAND_ACK_TIMEOUT)) {
            return false;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + LightSpaceNetConfig::COMMAND_ACK_TIMEOUT;
        if (waitForCommandAck(commandWord, deadline)) {
            return true;
        }
    }

    recordIntermittentError(error_types::network::timeout);
    return false;
}

bool LightSpaceNetController::hasTcpConnection() const {
    return tcpClient && tcpClient->is_connected();
}

bool LightSpaceNetController::waitForCommandAck(
    std::uint8_t commandWord,
    std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        bool ackMatched = false;
        if (!pollTcpIncomingPackets(commandWord, ackMatched)) {
            return false;
        }
        if (ackMatched) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool LightSpaceNetController::handleParsedIncomingPacket(
    const LightSpaceNetPacket& packet,
    std::uint8_t expectedAckCommand,
    bool& ackMatched) {
    ackMatched = false;

    if (packet.packetType == LightSpaceNetConfig::PACKET_TYPE_BASIC &&
        packet.commandWord == LightSpaceNetConfig::CMD_HEARTBEAT_RESPONSE) {
        lastHeartbeatReplyTime = std::chrono::steady_clock::now();
        heartbeatTimeoutLogged = false;
        return true;
    }

    if (timingLogEnabled &&
        packet.packetType == LightSpaceNetConfig::PACKET_TYPE_COMMAND &&
        packet.commandWord == LightSpaceNetConfig::CMD_COMMAND_ACK &&
        packet.payload.size() >= 2) {
        const auto acknowledged = readBe16(packet.payload.data());
        logInfo("[LightSpaceNetController] command ACK",
                "payload", acknowledged,
                "expected", static_cast<int>(expectedAckCommand));
    }

    if (expectedAckCommand != 0 && ackPayloadMatchesCommand(packet, expectedAckCommand)) {
        ackMatched = true;
        return true;
    }

    return true;
}

bool LightSpaceNetController::drainTcpPacketBuffer(std::uint8_t expectedAckCommand,
                                                   bool& ackMatched) {
    ackMatched = false;

    while (true) {
        const auto headerIt = std::search(
            tcpReceiveBuffer.begin(),
            tcpReceiveBuffer.end(),
            protocolHeader.begin(),
            protocolHeader.end());
        if (headerIt == tcpReceiveBuffer.end()) {
            if (tcpReceiveBuffer.size() > protocolHeader.size()) {
                // Keep a short suffix in case it is the beginning of the next
                // protocol header split across TCP reads.
                tcpReceiveBuffer.erase(
                    tcpReceiveBuffer.begin(),
                    tcpReceiveBuffer.end() -
                        static_cast<std::ptrdiff_t>(protocolHeader.size() - 1));
            }
            return true;
        }
        if (headerIt != tcpReceiveBuffer.begin()) {
            tcpReceiveBuffer.erase(tcpReceiveBuffer.begin(), headerIt);
        }

        if (tcpReceiveBuffer.size() < 12) {
            return true;
        }

        const auto packetSize = static_cast<std::size_t>(readBe16(tcpReceiveBuffer.data() + 10));
        if (packetSize < minimumPacketSize) {
            recordIntermittentError(error_types::network::protocolError);
            tcpReceiveBuffer.erase(tcpReceiveBuffer.begin());
            continue;
        }
        if (tcpReceiveBuffer.size() < packetSize) {
            return true;
        }

        auto packet = parsePacket(tcpReceiveBuffer.data(), packetSize);
        if (!packet) {
            recordIntermittentError(error_types::network::protocolError);
            tcpReceiveBuffer.erase(tcpReceiveBuffer.begin());
            continue;
        }

        bool packetAckMatched = false;
        const bool handled =
            handleParsedIncomingPacket(*packet, expectedAckCommand, packetAckMatched);
        tcpReceiveBuffer.erase(
            tcpReceiveBuffer.begin(),
            tcpReceiveBuffer.begin() + static_cast<std::ptrdiff_t>(packetSize));
        if (!handled) {
            return false;
        }
        if (packetAckMatched) {
            ackMatched = true;
            return true;
        }
    }
}

bool LightSpaceNetController::pollTcpIncomingPackets(std::uint8_t expectedAckCommand,
                                                     bool& ackMatched) {
    ackMatched = false;
    if (!tcpClient || !tcpClient->is_connected()) {
        return false;
    }

    bool bufferedAckMatched = false;
    if (!drainTcpPacketBuffer(expectedAckCommand, bufferedAckMatched)) {
        return false;
    }
    if (bufferedAckMatched) {
        ackMatched = true;
        return true;
    }

    constexpr int maxReadsPerPoll = 8;
    for (int i = 0; i < maxReadsPerPoll; ++i) {
        std::error_code ec;
        const auto available = tcpClient->getSocket().available(ec);
        if (ec) {
            logError("[LightSpaceNetController] TCP available failed", ec.message());
            recordConnectionError(error_types::network::receiveFailed);
            networkConnected.store(false, std::memory_order_relaxed);
            reconnectRequested.store(true, std::memory_order_relaxed);
            setConnectionState(false);
            return false;
        }
        if (available == 0) {
            return true;
        }

        std::array<std::uint8_t, 8192> buffer{};
        const auto readLimit = std::min<std::size_t>(available, buffer.size());
        const auto received =
            tcpClient->getSocket().read_some(net::asio::buffer(buffer.data(), readLimit), ec);
        if (ec) {
            logError("[LightSpaceNetController] TCP receive failed", ec.message());
            recordConnectionError(error_types::network::receiveFailed);
            networkConnected.store(false, std::memory_order_relaxed);
            reconnectRequested.store(true, std::memory_order_relaxed);
            setConnectionState(false);
            return false;
        }
        if (received == 0) {
            return true;
        }

        tcpReceiveBuffer.insert(tcpReceiveBuffer.end(), buffer.begin(), buffer.begin() +
            static_cast<std::ptrdiff_t>(received));

        bool packetAckMatched = false;
        if (!drainTcpPacketBuffer(expectedAckCommand, packetAckMatched)) {
            return false;
        }
        if (packetAckMatched) {
            ackMatched = true;
            return true;
        }
    }

    return true;
}

void LightSpaceNetController::pollIncomingPackets() {
    bool ackMatched = false;
    (void)pollTcpIncomingPackets(0, ackMatched);
}

void LightSpaceNetController::sendHeartbeatIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (lastHeartbeatSentTime == std::chrono::steady_clock::time_point{} ||
        now - lastHeartbeatSentTime >= LightSpaceNetConfig::HEARTBEAT_INTERVAL) {
        const auto packet = buildHeartbeatQueryPacket(steadyMillis());
        if (sendPacket(packet, std::chrono::milliseconds(50))) {
            lastHeartbeatSentTime = now;
        }
    }

    if (lastHeartbeatReplyTime != std::chrono::steady_clock::time_point{} &&
        now - lastHeartbeatReplyTime >= LightSpaceNetConfig::HEARTBEAT_DISCONNECT_AFTER) {
        if (!strictHeartbeat) {
            if (!heartbeatTimeoutLogged) {
                logInfo("[LightSpaceNetController] heartbeat timed out; keeping stream active");
                heartbeatTimeoutLogged = true;
            }
            return;
        }

        logError("[LightSpaceNetController] heartbeat timed out");
        recordConnectionError(error_types::network::connectionLost);
        networkConnected.store(false, std::memory_order_relaxed);
        reconnectRequested.store(true, std::memory_order_relaxed);
        setConnectionState(false);
    }
}

void LightSpaceNetController::syncPointRate() {
    const auto now = std::chrono::steady_clock::now();
    if (pointRatePushNeeded && nextPointRateSyncTime != std::chrono::steady_clock::time_point{} &&
        now < nextPointRateSyncTime) {
        return;
    }

    const auto desired = getPointRate();
    if (!pointRatePushNeeded && desired == lastSentPointRate) {
        return;
    }

    const auto scanRateKilohertz = LightSpaceNetConfig::scanFrequencyKilohertz(desired);
    logInfo("[LightSpaceNetController] setting point rate",
            desired,
            "pps scanRate",
            static_cast<int>(scanRateKilohertz),
            "kHz");

    const auto packet = buildScanFrequencyPacket(desired);
    if (sendControlCommand(LightSpaceNetConfig::CMD_SET_SCAN_FREQUENCY, packet)) {
        lastSentPointRate = desired;
        pointRatePushNeeded = false;
        nextPointRateSyncTime = {};
        logInfo("[LightSpaceNetController] point rate accepted",
                desired,
                "pps");
    } else {
        pointRatePushNeeded = true;
        nextPointRateSyncTime = now + commandRetryDelay;
        logError("[LightSpaceNetController] point rate command not acknowledged; retrying",
                 desired,
                 "pps");
    }
}

void LightSpaceNetController::syncLaserState() {
    const auto now = std::chrono::steady_clock::now();
    if (laserStatePushNeeded && nextLaserStateSyncTime != std::chrono::steady_clock::time_point{} &&
        now < nextLaserStateSyncTime) {
        return;
    }

    const bool armedNow = isArmed();
    if (!laserStatePushNeeded && armedNow == lastSentArmed) {
        return;
    }

    const auto packet = buildLaserSwitchPacket(armedNow);
    if (sendControlCommand(LightSpaceNetConfig::CMD_LASER_ON_OFF, packet)) {
        lastSentArmed = armedNow;
        laserStatePushNeeded = false;
        nextLaserStateSyncTime = {};
    } else {
        laserStatePushNeeded = true;
        nextLaserStateSyncTime = now + commandRetryDelay;
    }
}

void LightSpaceNetController::sendBlankPatternForShutdown() {
    const auto blankPoints = makeBlankPatternPoints(patternPointLimit);
    const auto packet = buildPointStreamPacket(blankPoints, coordinateOptions);
    for (int i = 0; i < 3; ++i) {
        (void)sendPacket(packet, std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool LightSpaceNetController::sendFramePattern() {
    const auto activePointRate = getPointRate();
    if (activePointRate == 0) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (nextPatternSendTime != std::chrono::steady_clock::time_point{} &&
        now < nextPatternSendTime) {
        lastSentPacketPointCount = 0;
        return true;
    }

    core::Frame frame;
    const auto estimatedFirstRenderTime =
        projectedNextWriteRenderTime(now, nonNegativeMicros(estimatedWriteLead));
    FrameFillRequest request{};
    request.maximumPointsRequired = std::numeric_limits<std::size_t>::max();
    request.preferredPointCount = patternPointLimit;
    request.blankFramePointCount = patternPointLimit;
    request.estimatedFirstPointRenderTime = estimatedFirstRenderTime;
    request.currentPointIndex = currentPointIndex;
    request.advanceWhenAvailable = true;

    if (!requestFrame(request, frame)) {
        return false;
    }
    if (frame.points.empty()) {
        lastSentPacketPointCount = 0;
        return true;
    }

    const auto& packetPoints = frame.points;
    if (packetPoints.empty()) {
        lastSentPacketPointCount = 0;
        return true;
    }

    std::vector<core::LaserPoint> fittedPatternPoints;
    const std::vector<core::LaserPoint>* pointsToSend = &packetPoints;
    if (packetPoints.size() > patternPointLimit) {
        if (isVerbose()) {
            logInfo("[LightSpaceNetController] source frame exceeds packet cap; fitting with blank travel tail",
                    "framePoints",
                    packetPoints.size(),
                    "cap",
                    patternPointLimit);
        }
        fittedPatternPoints = fitCurrentPatternToPointLimit(packetPoints, patternPointLimit);
        pointsToSend = &fittedPatternPoints;
    }

    const auto packet = buildPointStreamPacket(*pointsToSend, coordinateOptions);
    if (packet.empty() ||
        packet.size() > LightSpaceNetConfig::MAX_CURRENT_PATTERN_PACKET_BYTES) {
        logError("[LightSpaceNetController] refusing oversized current-pattern packet",
                 "points",
                 pointsToSend->size(),
                 "bytes",
                 packet.size(),
                 "maxBytes",
                 LightSpaceNetConfig::MAX_CURRENT_PATTERN_PACKET_BYTES);
        lastSentPacketPointCount = 0;
        return true;
    }

    const auto sendStart = std::chrono::steady_clock::now();
    const bool sent = sendPacket(packet, std::chrono::milliseconds(50));
    const auto sendDone = std::chrono::steady_clock::now();
    if (sent) {
        lastPatternSentTime = sendDone;
        lastSentPacketPointCount = pointsToSend->size();
        lastSentPacketBytes = packet.size();
        const auto playbackDuration =
            pointPlaybackDuration(lastSentPacketPointCount, activePointRate);
        const auto minimumInterval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                patternUpdateInterval);
        nextPatternSendTime = sendDone + std::max(playbackDuration, minimumInterval);
        currentPointIndex += lastSentPacketPointCount;
        recordLatencySample(sendDone - sendStart);
        estimatedWriteLead = smoothWriteLead(estimatedWriteLead, sendDone - sendStart);
        noteFrameTransportSubmissionBounded(
            lastSentPacketPointCount,
            request.estimatedFirstPointRenderTime,
            activePointRate,
            lastSubmittedPatternPoints);
        lastSubmittedPatternPoints = lastSentPacketPointCount;
        recordTimingSample(
            lastSentPacketPointCount,
            activePointRate);
    }
    return sent;
}

void LightSpaceNetController::recordTimingSample(std::size_t sentPointCount,
                                                 std::uint32_t activePointRate) {
    if (!timingLogEnabled) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (timingLogWindowStart == std::chrono::steady_clock::time_point{}) {
        timingLogWindowStart = now;
    }

    if (sentPointCount > 0) {
        ++timingLogPacketsSent;
        timingLogPointsSent += sentPointCount;
    }

    if (now - timingLogWindowStart < std::chrono::seconds(1)) {
        return;
    }

    const auto elapsedSeconds =
        std::chrono::duration<double>(now - timingLogWindowStart).count();
    const auto measuredPointRate = elapsedSeconds > 0.0
        ? static_cast<std::uint32_t>(
              std::llround(static_cast<double>(timingLogPointsSent) / elapsedSeconds))
        : 0u;

    // This is a host-side measurement of the TCP upload cadence. The DAC still
    // controls the actual display timing after accepting each complete pattern.
    logInfo("[LightSpaceNetController] timing",
            "targetPps", activePointRate,
            "hostSentPps", measuredPointRate,
            "packets", timingLogPacketsSent,
            "points", timingLogPointsSent,
            "lastBytes", lastSentPacketBytes,
            "lastPacket", sentPointCount);

    timingLogWindowStart = now;
    timingLogPacketsSent = 0;
    timingLogPointsSent = 0;
}

} // namespace libera::lightspacenet
