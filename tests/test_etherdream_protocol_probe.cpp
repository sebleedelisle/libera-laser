#include "libera/core/ByteRead.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/net/NetService.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/net/UdpSocket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t DEFAULT_POINT_RATE = 30000;
constexpr std::uint32_t RATE_CHANGE_TARGET = 12000;
constexpr std::uint16_t SMALL_PACKET_POINTS = 200;
constexpr std::uint16_t RATE_CHANGE_MAX_PREROLL_POINTS = 1600;
constexpr std::uint16_t RATE_CHANGE_MAX_PACKET_POINTS = 1000;
constexpr std::uint16_t UNDERFLOW_PACKET_POINTS = 200;

enum class ProbeStatus {
    Pass,
    Mismatch,
    Observation,
    Skipped
};

struct ProbeResult {
    ProbeStatus status = ProbeStatus::Observation;
    std::string name;
    std::string detail;
};

struct Options {
    std::string ip;
    bool includeEstop = false;
    bool includeExclusive = false;
    bool stressRateQueue = false;
    bool probeDataFullThreshold = false;
    bool badDataCrashProbe = false;
    std::uint16_t maxDataFullThresholdPoints = 8192;
};

struct BroadcastSample {
    std::string ip;
    std::string mac;
    std::uint16_t hardwareRevision = 0;
    std::uint16_t softwareRevision = 0;
    std::uint16_t bufferCapacity = 0;
    std::uint32_t maxPointRate = 0;
    libera::etherdream::EtherDreamStatus status;
    std::chrono::steady_clock::time_point receivedAt;
};

struct CommandResponse {
    bool received = false;
    std::error_code error;
    libera::etherdream::EtherDreamResponse response;
    std::chrono::steady_clock::duration elapsed{};
};

struct BadDataCase {
    std::string name;
    std::vector<std::uint8_t> payload;
    bool prepareFirst = false;
};

std::string hexByte(std::uint8_t value) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
       << static_cast<unsigned>(value);
    return os.str();
}

std::string commandName(std::uint8_t command) {
    if (command >= 32 && command <= 126) {
        return std::string("'") + static_cast<char>(command) + "'/" + hexByte(command);
    }
    return hexByte(command);
}

std::string responseName(const CommandResponse& response) {
    if (!response.received) {
        return "no response";
    }
    return std::string("'") + static_cast<char>(response.response.response) + "' for " +
           commandName(response.response.command);
}

std::string statusLine(const libera::etherdream::EtherDreamStatus& status) {
    std::ostringstream os;
    os << status.describe()
       << " source=" << static_cast<unsigned>(status.source)
       << " playback_flags=0x" << std::hex << std::uppercase << status.playbackFlags
       << " light_flags=0x" << status.lightEngineFlags
       << " source_flags=0x" << status.sourceFlags
       << std::dec << std::nouppercase;
    return os.str();
}

std::string macString(const std::uint8_t* data) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (int i = 0; i < 6; ++i) {
        if (i) {
            os << ':';
        }
        os << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return os.str();
}

libera::etherdream::EtherDreamStatus decodeStatusBytes(const std::uint8_t* data) {
    libera::etherdream::EtherDreamStatus status;
    status.protocol = data[0];
    status.lightEngineState = static_cast<libera::etherdream::LightEngineState>(data[1]);
    status.playbackState = static_cast<libera::etherdream::PlaybackState>(data[2]);
    status.source = data[3];
    status.lightEngineFlags = libera::core::bytes::readLe16(data + 4);
    status.playbackFlags = libera::core::bytes::readLe16(data + 6);
    status.sourceFlags = libera::core::bytes::readLe16(data + 8);
    status.bufferFullness = libera::core::bytes::readLe16(data + 10);
    status.pointRate = libera::core::bytes::readLe32(data + 12);
    status.pointCount = libera::core::bytes::readLe32(data + 16);
    return status;
}

void appendLe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void appendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

std::vector<std::uint8_t> singleByteCommand(std::uint8_t command) {
    return {command};
}

std::vector<std::uint8_t> beginCommand(std::uint32_t pointRate,
                                       std::uint16_t lowWaterMark = 0) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>('b'));
    appendLe16(out, lowWaterMark);
    appendLe32(out, pointRate);
    return out;
}

std::vector<std::uint8_t> rateCommand(std::uint32_t pointRate) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>('q'));
    appendLe32(out, pointRate);
    return out;
}

std::vector<std::uint8_t> dataCommand(std::uint16_t pointCount,
                                      bool firstPointChangesRate = false) {
    std::vector<std::uint8_t> out;
    out.reserve(3u + static_cast<std::size_t>(pointCount) * 18u);
    out.push_back(static_cast<std::uint8_t>('d'));
    appendLe16(out, pointCount);
    for (std::uint16_t i = 0; i < pointCount; ++i) {
        appendLe16(out, (firstPointChangesRate && i == 0) ? 0x8000u : 0u);
        appendLe16(out, 0); // x
        appendLe16(out, 0); // y
        appendLe16(out, 0); // r
        appendLe16(out, 0); // g
        appendLe16(out, 0); // b
        appendLe16(out, 0); // i
        appendLe16(out, 0); // u1
        appendLe16(out, 0); // u2
    }
    return out;
}

std::chrono::milliseconds drainTimeFor(std::uint16_t points,
                                       std::uint32_t pointRate,
                                       std::chrono::milliseconds extra = 0ms) {
    if (pointRate == 0) {
        return extra;
    }
    const auto micros = static_cast<long long>(
        (static_cast<double>(points) / static_cast<double>(pointRate)) * 1'000'000.0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds(micros)) +
           extra;
}

std::uint16_t clampU16(std::uint32_t value,
                       std::uint32_t minimum,
                       std::uint32_t maximum) {
    return static_cast<std::uint16_t>(std::clamp(value, minimum, maximum));
}

std::string joinIntervals(const std::vector<std::chrono::milliseconds>& intervals) {
    std::ostringstream os;
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        if (i) {
            os << ", ";
        }
        os << intervals[i].count() << "ms";
    }
    return os.str();
}

class ProtocolProbe {
public:
    explicit ProtocolProbe(Options options)
    : options(std::move(options)) {}

    int run() {
        listenForBroadcasts();
        if (options.ip.empty() && !broadcasts.empty()) {
            options.ip = broadcasts.front().ip;
        }

        if (options.ip.empty()) {
            add(ProbeStatus::Skipped,
                "TCP command probes",
                "No Ether Dream broadcast was found and no IP was supplied.");
            printSummary();
            return 2;
        }

        if (!connectTcp()) {
            printSummary();
            return 2;
        }

        runSafeCommandProbes();

        if (options.stressRateQueue) {
            runRateQueueStressProbe();
        } else {
            add(ProbeStatus::Skipped,
                "NAK Full for rate queue",
                "Use --stress-rate-queue to repeatedly queue point rates until the DAC replies F q.");
        }

        if (options.probeDataFullThreshold) {
            runDataFullThresholdProbe();
        } else {
            add(ProbeStatus::Skipped,
                "NAK Full data threshold",
                "Use --probe-data-full-threshold to send larger blank data packets until F d or the safety limit.");
        }

        if (options.includeEstop) {
            runEstopProbes();
        } else {
            add(ProbeStatus::Skipped,
                "Emergency stop commands",
                "Use --include-estop to send 0x00/0xFF and then clear E-stop.");
        }

        if (options.includeExclusive) {
            runExclusiveConnectionProbe();
        } else {
            add(ProbeStatus::Skipped,
                "One-host connection exclusivity",
                "Use --include-exclusive to try a second TCP connection while this probe owns the first.");
        }

        sendAndExpect("cleanup stop", singleByteCommand('s'), 's');
        client.close();

        if (options.badDataCrashProbe) {
            runBadDataCrashProbe();
        } else {
            add(ProbeStatus::Skipped,
                "Bad data crash probe",
                "Use --bad-data-crash-probe to send malformed TCP command streams to the DAC.");
        }

        printSummary();
        return 0;
    }

private:
    void add(ProbeStatus status, std::string name, std::string detail) {
        results.push_back(ProbeResult{status, std::move(name), std::move(detail)});
    }

    void expect(std::string name, bool condition, std::string detail) {
        add(condition ? ProbeStatus::Pass : ProbeStatus::Mismatch,
            std::move(name),
            std::move(detail));
    }

    static bool isAckFor(const CommandResponse& response, std::uint8_t command) {
        return response.received
            && response.response.response == static_cast<std::uint8_t>('a')
            && response.response.command == command;
    }

    static bool isInvalidFor(const CommandResponse& response, std::uint8_t command) {
        return response.received
            && response.response.response == static_cast<std::uint8_t>('I')
            && response.response.command == command;
    }

    static bool isFullFor(const CommandResponse& response, std::uint8_t command) {
        return response.received
            && response.response.response == static_cast<std::uint8_t>('F')
            && response.response.command == command;
    }

    void listenForBroadcasts() {
        auto io = libera::net::shared_io_context();
        libera::net::UdpSocket socket(*io);
        std::error_code ec = socket.open_v4(false);
        if (ec) {
            add(ProbeStatus::Skipped, "UDP broadcast", "Could not open UDP socket: " + ec.message());
            return;
        }

        socket.raw().set_option(libera::net::asio::socket_base::reuse_address(true), ec);
        ec = socket.bind_any(libera::etherdream::config::ETHERDREAM_DISCOVERY_PORT, false);
        if (ec) {
            add(ProbeStatus::Skipped,
                "UDP broadcast",
                "Could not bind discovery port 7654, probably already exclusively owned: " + ec.message());
            socket.close();
            return;
        }

        const auto deadline = std::chrono::steady_clock::now() + 4200ms;
        while (std::chrono::steady_clock::now() < deadline && broadcasts.size() < 5) {
            std::array<std::uint8_t, 128> packet{};
            libera::net::udp::endpoint sender;
            std::size_t received = 0;
            ec = socket.recv_from(packet.data(), packet.size(), sender, received, 1200ms, false);
            if (ec) {
                continue;
            }
            if (received < 36) {
                add(ProbeStatus::Mismatch,
                    "Broadcast packet size",
                    "Received " + std::to_string(received) + " bytes, expected at least 36.");
                continue;
            }

            BroadcastSample sample;
            sample.ip = sender.address().to_string();
            if (!options.ip.empty() && sample.ip != options.ip) {
                continue;
            }
            sample.mac = macString(packet.data());
            const auto* cursor = packet.data() + 6;
            sample.hardwareRevision = libera::core::bytes::readLe16(cursor); cursor += 2;
            sample.softwareRevision = libera::core::bytes::readLe16(cursor); cursor += 2;
            sample.bufferCapacity = libera::core::bytes::readLe16(cursor); cursor += 2;
            sample.maxPointRate = libera::core::bytes::readLe32(cursor); cursor += 4;
            sample.status = decodeStatusBytes(cursor);
            sample.receivedAt = std::chrono::steady_clock::now();
            broadcasts.push_back(sample);
        }

        socket.close();
        if (broadcasts.empty()) {
            add(ProbeStatus::Skipped,
                "UDP broadcast",
                options.ip.empty()
                    ? "No Ether Dream broadcasts were received."
                    : "No Ether Dream broadcasts were received from " + options.ip + ".");
            return;
        }

        const auto& first = broadcasts.front();
        bufferCapacity = first.bufferCapacity;
        maxPointRate = first.maxPointRate;
        std::ostringstream detail;
        detail << "saw " << broadcasts.size()
               << " broadcast(s), ip=" << first.ip
               << " mac=" << first.mac
               << " hw=" << first.hardwareRevision
               << " sw=" << first.softwareRevision
               << " buffer_capacity=" << first.bufferCapacity
               << " max_point_rate=" << first.maxPointRate
               << " status={" << statusLine(first.status) << "}";
        add(ProbeStatus::Pass, "UDP broadcast struct", detail.str());

        if (broadcasts.size() >= 2) {
            std::vector<std::chrono::milliseconds> intervals;
            for (std::size_t i = 1; i < broadcasts.size(); ++i) {
                intervals.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                    broadcasts[i].receivedAt - broadcasts[i - 1].receivedAt));
            }
            const auto hasOneSecondInterval = std::any_of(
                intervals.begin(),
                intervals.end(),
                [](std::chrono::milliseconds interval) {
                    return interval >= 600ms && interval <= 1600ms;
                });
            expect("UDP broadcast cadence",
                   hasOneSecondInterval,
                   "captured intervals: " + joinIntervals(intervals));
        } else {
            add(ProbeStatus::Observation,
                "UDP broadcast cadence",
                "Only one broadcast was captured in the short probe window.");
        }
    }

    bool connectTcp() {
        std::error_code ec;
        const auto address = libera::net::asio::ip::make_address(options.ip, ec);
        if (ec) {
            add(ProbeStatus::Skipped, "TCP connect", "Invalid IP " + options.ip + ": " + ec.message());
            return false;
        }

        client.setConnectTimeout(700ms);
        client.setDefaultTimeout(700ms);
        const libera::net::tcp::endpoint endpoint(
            address,
            libera::etherdream::config::ETHERDREAM_DAC_PORT_DEFAULT);
        ec = client.connect(endpoint);
        if (ec) {
            add(ProbeStatus::Skipped, "TCP connect", "Connect failed: " + ec.message());
            return false;
        }
        client.setLowLatency();
        add(ProbeStatus::Pass, "TCP connect", "connected to " + options.ip + ":7765");

        const auto initial = readResponse(800ms);
        expect("Initial status frame on connect",
               isAckFor(initial, '?'),
               responseName(initial) + " status={" +
                   (initial.received ? statusLine(initial.response.status) : std::string{}) + "}");
        return true;
    }

    CommandResponse readResponseFrom(libera::net::TcpClient& target,
                                     std::chrono::milliseconds timeout) {
        CommandResponse result;
        std::array<std::uint8_t, 22> raw{};
        const auto start = std::chrono::steady_clock::now();
        std::size_t bytes = 0;
        result.error = target.read_exact(raw.data(), raw.size(), timeout, &bytes);
        result.elapsed = std::chrono::steady_clock::now() - start;
        if (result.error) {
            return result;
        }
        result.received = result.response.decode(raw.data(), raw.size());
        if (!result.received) {
            result.error = std::make_error_code(std::errc::protocol_error);
        }
        return result;
    }

    CommandResponse readResponse(std::chrono::milliseconds timeout) {
        return readResponseFrom(client, timeout);
    }

    CommandResponse sendAndReadOn(libera::net::TcpClient& target,
                                  const std::vector<std::uint8_t>& command,
                                  std::uint8_t expectedCommand,
                                  std::chrono::milliseconds timeout = 700ms) {
        CommandResponse result;
        if (command.empty()) {
            result.error = std::make_error_code(std::errc::invalid_argument);
            return result;
        }

        auto ec = target.write_all(command.data(), command.size(), timeout);
        if (ec) {
            result.error = ec;
            return result;
        }

        const auto start = std::chrono::steady_clock::now();
        for (int attempt = 0; attempt < 8; ++attempt) {
            result = readResponseFrom(target, timeout);
            result.elapsed = std::chrono::steady_clock::now() - start;
            if (!result.received) {
                return result;
            }
            if (result.response.command == expectedCommand) {
                return result;
            }
        }

        result.error = std::make_error_code(std::errc::protocol_error);
        return result;
    }

    CommandResponse sendAndRead(const std::string& label,
                                const std::vector<std::uint8_t>& command,
                                std::uint8_t expectedCommand) {
        CommandResponse result;
        if (command.empty()) {
            result.error = std::make_error_code(std::errc::invalid_argument);
            return result;
        }

        auto ec = client.write_all(command.data(), command.size(), 700ms);
        if (ec) {
            result.error = ec;
            return result;
        }

        const auto start = std::chrono::steady_clock::now();
        for (int attempt = 0; attempt < 8; ++attempt) {
            result = readResponse(700ms);
            result.elapsed = std::chrono::steady_clock::now() - start;
            if (!result.received) {
                return result;
            }
            if (result.response.command == expectedCommand) {
                return result;
            }
            std::printf("OBSERVE %-35s ignored unmatched %s while waiting for %s\n",
                        label.c_str(),
                        responseName(result).c_str(),
                        commandName(expectedCommand).c_str());
        }

        result.error = std::make_error_code(std::errc::protocol_error);
        return result;
    }

    CommandResponse sendAndExpect(const std::string& name,
                                  const std::vector<std::uint8_t>& command,
                                  std::uint8_t expectedCommand) {
        const auto response = sendAndRead(name, command, expectedCommand);
        if (!response.received) {
            add(ProbeStatus::Mismatch,
                name,
                "No matching response for " + commandName(expectedCommand) +
                    ", error=" + response.error.message());
        }
        return response;
    }

    void sendStopToIdle() {
        const auto stop = sendAndRead("force stop", singleByteCommand('s'), 's');
        if (stop.received && stop.response.response == static_cast<std::uint8_t>('a')) {
            return;
        }
        if (stop.received && stop.response.response == static_cast<std::uint8_t>('I')) {
            return;
        }
    }

    void runSafeCommandProbes() {
        const auto ping = sendAndExpect("Ping ACK", singleByteCommand('?'), '?');
        expect("Ping command",
               isAckFor(ping, '?'),
               responseName(ping) + " status={" +
                   (ping.received ? statusLine(ping.response.status) : std::string{}) + "}");

        sendStopToIdle();

        const auto stopIdle = sendAndExpect("Stop invalid while idle", singleByteCommand('s'), 's');
        expect("Stop invalid while idle",
               isInvalidFor(stopIdle, 's'),
               responseName(stopIdle) + " status={" +
                   (stopIdle.received ? statusLine(stopIdle.response.status) : std::string{}) + "}");

        const auto clearReady = sendAndExpect("Clear invalid while not E-stop", singleByteCommand('c'), 'c');
        expect("Clear invalid while not E-stop",
               isInvalidFor(clearReady, 'c'),
               responseName(clearReady) + " status={" +
                   (clearReady.received ? statusLine(clearReady.response.status) : std::string{}) + "}");

        const auto prepare = sendAndExpect("Prepare from idle", singleByteCommand('p'), 'p');
        expect("Prepare from idle ACK",
               isAckFor(prepare, 'p'),
               responseName(prepare) + " status={" +
                   (prepare.received ? statusLine(prepare.response.status) : std::string{}) + "}");
        if (prepare.received) {
            expect("Prepare empties buffer",
                   prepare.response.status.bufferFullness == 0,
                   "buffer_fullness=" + std::to_string(prepare.response.status.bufferFullness));
            expect("Prepare resets point_count to zero",
                   prepare.response.status.pointCount == 0,
                   "point_count=" + std::to_string(prepare.response.status.pointCount));
            expect("Prepared source is network",
                   prepare.response.status.source == 0,
                   "source=" + std::to_string(prepare.response.status.source));
        }

        const auto prepareAgain = sendAndExpect("Prepare invalid while prepared", singleByteCommand('p'), 'p');
        expect("Prepare invalid while prepared",
               isInvalidFor(prepareAgain, 'p'),
               responseName(prepareAgain));

        const auto zeroData = sendAndExpect("Zero-point data while prepared", dataCommand(0), 'd');
        expect("Zero-point data while prepared",
               isAckFor(zeroData, 'd'),
               responseName(zeroData) + " status={" +
                   (zeroData.received ? statusLine(zeroData.response.status) : std::string{}) + "}");

        const auto beginNoData = sendAndExpect("Begin invalid with empty buffer", beginCommand(DEFAULT_POINT_RATE), 'b');
        expect("Begin invalid with empty buffer",
               isInvalidFor(beginNoData, 'b'),
               responseName(beginNoData) + " status={" +
                   (beginNoData.received ? statusLine(beginNoData.response.status) : std::string{}) + "}");

        sendStopToIdle();
        const auto prepareAfterEmptyBegin = sendAndExpect("Prepare after empty-begin check",
                                                         singleByteCommand('p'),
                                                         'p');
        expect("Prepare after empty-begin check",
               isAckFor(prepareAfterEmptyBegin, 'p'),
               responseName(prepareAfterEmptyBegin) + " status={" +
                   (prepareAfterEmptyBegin.received
                        ? statusLine(prepareAfterEmptyBegin.response.status)
                        : std::string{}) + "}");
        if (!isAckFor(prepareAfterEmptyBegin, 'p')) {
            add(ProbeStatus::Skipped,
                "Prepared-state probes",
                "Could not reset the stream after begin-with-empty-buffer check.");
            runUnderflowProbe();
            return;
        }

        const auto qPrepared = sendAndExpect("Queue point-rate while prepared", rateCommand(DEFAULT_POINT_RATE), 'q');
        expect("Queue point-rate while prepared",
               isAckFor(qPrepared, 'q'),
               responseName(qPrepared) + "; opcode tested was 'q'/0x71. The PDF's 0x74 value appears suspect.");

        sendStopToIdle();
        const auto prepareAfterQPrepared = sendAndExpect("Prepare after point-rate validity check",
                                                         singleByteCommand('p'),
                                                         'p');
        expect("Prepare after point-rate validity check",
               isAckFor(prepareAfterQPrepared, 'p'),
               responseName(prepareAfterQPrepared) + " status={" +
                   (prepareAfterQPrepared.received
                        ? statusLine(prepareAfterQPrepared.response.status)
                        : std::string{}) + "}");
        if (!isAckFor(prepareAfterQPrepared, 'p')) {
            add(ProbeStatus::Skipped,
                "Streaming and buffer probes",
                "Could not reset the stream after checking q while prepared.");
            runUnderflowProbe();
            return;
        }

        const auto smallData = sendAndExpect("Small data packet fits", dataCommand(SMALL_PACKET_POINTS), 'd');
        expect("Small data packet fits",
               isAckFor(smallData, 'd'),
               responseName(smallData) + " status={" +
                   (smallData.received ? statusLine(smallData.response.status) : std::string{}) + "}");

        sendStopToIdle();
        const auto prepareForStreaming = sendAndExpect("Prepare before streaming probes",
                                                       singleByteCommand('p'),
                                                       'p');
        if (!isAckFor(prepareForStreaming, 'p')) {
            add(ProbeStatus::Skipped,
                "Streaming probes",
                "Could not prepare the stream after the small packet probe.");
            runUnderflowProbe();
            return;
        }

        const std::uint32_t discoveredCapacity = bufferCapacity > 0 ? bufferCapacity : 4096u;
        const std::uint16_t rateChangePrerollPoints = clampU16(
            discoveredCapacity / 2u,
            300u,
            RATE_CHANGE_MAX_PREROLL_POINTS);
        const std::uint16_t rateChangePacketPoints = clampU16(
            discoveredCapacity / 4u,
            200u,
            RATE_CHANGE_MAX_PACKET_POINTS);

        const auto prerollData = sendAndExpect("Rate-change preroll data",
                                               dataCommand(rateChangePrerollPoints),
                                               'd');
        expect("Rate-change preroll data",
               isAckFor(prerollData, 'd'),
               responseName(prerollData) + " points=" + std::to_string(rateChangePrerollPoints) +
                   " status={" +
                   (prerollData.received ? statusLine(prerollData.response.status) : std::string{}) + "}");

        constexpr std::uint16_t LOW_WATER_MARK_PROBE = 1234;
        const auto begin = sendAndExpect("Begin with data", beginCommand(DEFAULT_POINT_RATE,
                                                                         LOW_WATER_MARK_PROBE), 'b');
        expect("Begin with data",
               isAckFor(begin, 'b')
                   && begin.response.status.playbackState == libera::etherdream::PlaybackState::Playing,
               responseName(begin) + " status={" +
                   (begin.received ? statusLine(begin.response.status) : std::string{}) + "}");
        if (begin.received) {
            add(ProbeStatus::Observation,
                "Begin low-water mark",
                "begin accepted low_water_mark=" + std::to_string(LOW_WATER_MARK_PROBE) +
                    " status={" + statusLine(begin.response.status) + "}");
        }

        const auto qPlaying = sendAndExpect("Queue point-rate while playing", rateCommand(RATE_CHANGE_TARGET), 'q');
        expect("Queue point-rate while playing",
               isAckFor(qPlaying, 'q'),
               responseName(qPlaying));

        const auto rateChangeData = sendAndExpect("Rate-change control bit",
                                                  dataCommand(rateChangePacketPoints, true),
                                                  'd');
        expect("Rate-change data ACK",
               isAckFor(rateChangeData, 'd'),
               responseName(rateChangeData) + " points=" + std::to_string(rateChangePacketPoints));

        const std::uint16_t bufferedBeforeRateChange =
            begin.received ? begin.response.status.bufferFullness : rateChangePrerollPoints;
        std::this_thread::sleep_for(
            drainTimeFor(static_cast<std::uint16_t>(bufferedBeforeRateChange + 2u),
                         DEFAULT_POINT_RATE,
                         20ms));
        const auto rateProbe = sendAndExpect("Rate changed after flagged point", singleByteCommand('?'), '?');
        expect("Rate changed after flagged point",
               rateProbe.received && rateProbe.response.status.pointRate == RATE_CHANGE_TARGET,
               responseName(rateProbe) + " status={" +
                   (rateProbe.received ? statusLine(rateProbe.response.status) : std::string{}) + "}");

        const auto stopPlaying = sendAndExpect("Stop while playing/prepared", singleByteCommand('s'), 's');
        expect("Stop while playing/prepared",
               isAckFor(stopPlaying, 's')
                   && stopPlaying.response.status.playbackState == libera::etherdream::PlaybackState::Idle,
               responseName(stopPlaying) + " status={" +
                   (stopPlaying.received ? statusLine(stopPlaying.response.status) : std::string{}) + "}");

        const auto dataIdle = sendAndExpect("Data invalid while idle", dataCommand(1), 'd');
        expect("Data invalid while idle",
               isInvalidFor(dataIdle, 'd'),
               responseName(dataIdle) + " status={" +
                   (dataIdle.received ? statusLine(dataIdle.response.status) : std::string{}) + "}");

        const auto qIdle = sendAndExpect("Queue point-rate invalid while idle", rateCommand(DEFAULT_POINT_RATE), 'q');
        expect("Queue point-rate invalid while idle",
               isInvalidFor(qIdle, 'q'),
               responseName(qIdle) + " status={" +
                   (qIdle.received ? statusLine(qIdle.response.status) : std::string{}) + "}");

        const auto beginIdle = sendAndExpect("Begin invalid while idle", beginCommand(DEFAULT_POINT_RATE), 'b');
        expect("Begin invalid while idle",
               isInvalidFor(beginIdle, 'b'),
               responseName(beginIdle));

        const auto preparedForFull = sendAndExpect("Prepare before full-buffer probe", singleByteCommand('p'), 'p');
        if (isAckFor(preparedForFull, 'p')) {
            const std::uint32_t capacity = bufferCapacity > 0 ? bufferCapacity : 4096;
            const std::uint16_t oversizedCount =
                static_cast<std::uint16_t>(std::min<std::uint32_t>(capacity + 1u, 65535u));
            const auto fullData = sendAndExpect("Data packet larger than buffer", dataCommand(oversizedCount), 'd');
            expect("NAK Full for oversized data",
                   isFullFor(fullData, 'd'),
                   responseName(fullData) + " oversized_points=" + std::to_string(oversizedCount) +
                       " known_capacity=" + std::to_string(capacity) +
                       " status={" + (fullData.received ? statusLine(fullData.response.status) : std::string{}) + "}");
        } else {
            add(ProbeStatus::Skipped,
                "NAK Full for oversized data",
                "Could not prepare the stream first.");
        }

        runUnderflowProbe();
    }

    void runUnderflowProbe() {
        sendStopToIdle();
        const auto prepare = sendAndExpect("Prepare before starvation", singleByteCommand('p'), 'p');
        if (!isAckFor(prepare, 'p')) {
            add(ProbeStatus::Skipped, "Underflow/starvation report", "Prepare failed.");
            return;
        }

        const auto data = sendAndExpect("Starvation data", dataCommand(UNDERFLOW_PACKET_POINTS), 'd');
        const auto begin = sendAndExpect("Starvation begin", beginCommand(DEFAULT_POINT_RATE), 'b');
        if (!isAckFor(data, 'd') || !isAckFor(begin, 'b')) {
            add(ProbeStatus::Skipped, "Underflow/starvation report", "Could not start short stream.");
            return;
        }

        std::this_thread::sleep_for(drainTimeFor(UNDERFLOW_PACKET_POINTS, DEFAULT_POINT_RATE, 150ms));
        const auto afterDrain = sendAndExpect("Status after starvation", singleByteCommand('?'), '?');
        expect("Starvation returns playback to idle",
               afterDrain.received
                   && afterDrain.response.status.playbackState == libera::etherdream::PlaybackState::Idle,
               responseName(afterDrain) + " status={" +
                   (afterDrain.received ? statusLine(afterDrain.response.status) : std::string{}) + "}");
        expect("Underflow flag after deliberate starvation",
               afterDrain.received && afterDrain.response.status.hasPlaybackUnderflow(),
               responseName(afterDrain) + " status={" +
                   (afterDrain.received ? statusLine(afterDrain.response.status) : std::string{}) + "}");
        expect("Idle point_count is zero",
               afterDrain.received && afterDrain.response.status.pointCount == 0,
               responseName(afterDrain) + " status={" +
                   (afterDrain.received ? statusLine(afterDrain.response.status) : std::string{}) + "}");

        const auto dataAfterIdle = sendAndExpect("Data after starvation idle", dataCommand(1), 'd');
        expect("Data after starvation idle is invalid",
               isInvalidFor(dataAfterIdle, 'd'),
               responseName(dataAfterIdle) + " status={" +
                   (dataAfterIdle.received ? statusLine(dataAfterIdle.response.status) : std::string{}) + "}");

        const auto prepareAgain = sendAndExpect("Prepare clears playback flags", singleByteCommand('p'), 'p');
        expect("Prepare clears playback flags",
               prepareAgain.received && prepareAgain.response.status.playbackFlags == 0,
               responseName(prepareAgain) + " status={" +
                   (prepareAgain.received ? statusLine(prepareAgain.response.status) : std::string{}) + "}");
    }

    void runRateQueueStressProbe() {
        sendStopToIdle();
        const auto prepare = sendAndExpect("Prepare before rate-queue stress", singleByteCommand('p'), 'p');
        if (!isAckFor(prepare, 'p')) {
            add(ProbeStatus::Skipped, "NAK Full for rate queue", "Could not prepare stream.");
            return;
        }

        bool sawFull = false;
        int commandsSent = 0;
        for (; commandsSent < 256; ++commandsSent) {
            const auto response = sendAndExpect(
                "rate queue stress",
                rateCommand(static_cast<std::uint32_t>(10000 + (commandsSent % 1000))),
                'q');
            if (isFullFor(response, 'q')) {
                sawFull = true;
                ++commandsSent;
                break;
            }
            if (!isAckFor(response, 'q')) {
                ++commandsSent;
                break;
            }
        }

        if (sawFull) {
            add(ProbeStatus::Pass,
                "NAK Full for rate queue",
                "F q appeared after " + std::to_string(commandsSent) + " q command(s)");
        } else {
            add(ProbeStatus::Observation,
                "NAK Full for rate queue",
                "No F q after " + std::to_string(commandsSent) + " q command(s)");
        }
        sendStopToIdle();
    }

    void runDataFullThresholdProbe() {
        const std::uint32_t advertisedCapacity = bufferCapacity > 0 ? bufferCapacity : 4096u;
        const std::uint16_t maxPoints =
            static_cast<std::uint16_t>(std::max<std::uint32_t>(
                advertisedCapacity + 1u,
                options.maxDataFullThresholdPoints));

        std::vector<std::uint16_t> counts;
        const auto addCount = [&](std::uint32_t count) {
            if (count <= advertisedCapacity || count > maxPoints || count > 65535u) {
                return;
            }
            const auto value = static_cast<std::uint16_t>(count);
            if (std::find(counts.begin(), counts.end(), value) == counts.end()) {
                counts.push_back(value);
            }
        };

        addCount(advertisedCapacity + 1u);
        addCount(4096u);
        addCount(4608u);
        addCount(5120u);
        addCount(6144u);
        addCount(8192u);
        addCount(maxPoints);
        std::sort(counts.begin(), counts.end());

        bool sawFull = false;
        bool stoppedEarly = false;
        std::uint16_t largestAcked = 0;
        std::uint16_t firstFull = 0;
        std::uint16_t lastAttempted = 0;
        std::string stopReason;
        for (const auto count : counts) {
            lastAttempted = count;
            sendStopToIdle();
            const auto prepare = sendAndExpect("Prepare before data-full threshold", singleByteCommand('p'), 'p');
            if (!isAckFor(prepare, 'p')) {
                stoppedEarly = true;
                stopReason = "prepare failed before " + std::to_string(count) + "-point packet";
                add(ProbeStatus::Skipped,
                    "NAK Full data threshold",
                    stopReason + ".");
                break;
            }

            const auto response = sendAndExpect("Data-full threshold " + std::to_string(count),
                                                dataCommand(count),
                                                'd');
            if (isFullFor(response, 'd')) {
                sawFull = true;
                firstFull = count;
                add(ProbeStatus::Pass,
                    "NAK Full data threshold",
                    "first F d at " + std::to_string(firstFull) +
                        " points; largest ACK was " + std::to_string(largestAcked) +
                        "; advertised_capacity=" + std::to_string(advertisedCapacity) +
                        " status={" + statusLine(response.response.status) + "}");
                break;
            }
            if (isAckFor(response, 'd')) {
                largestAcked = count;
                add(ProbeStatus::Observation,
                    "Data packet above advertised capacity",
                    "ACK d for " + std::to_string(count) +
                        " points; advertised_capacity=" + std::to_string(advertisedCapacity) +
                        " status={" + statusLine(response.response.status) + "}");
                continue;
            }
            add(ProbeStatus::Mismatch,
                "NAK Full data threshold",
                responseName(response) + " for " + std::to_string(count) +
                    " points; advertised_capacity=" + std::to_string(advertisedCapacity) +
                    " status={" + (response.received ? statusLine(response.response.status) : std::string{}) + "}");
            stoppedEarly = true;
            stopReason = responseName(response) + " at " + std::to_string(count) + " points";
            break;
        }

        if (!sawFull) {
            if (stoppedEarly) {
                add(ProbeStatus::Observation,
                    "NAK Full data threshold",
                    "No F d before probe stopped: " + stopReason +
                        "; last_attempted=" + std::to_string(lastAttempted) +
                        "; largest ACK was " + std::to_string(largestAcked) +
                        "; advertised_capacity=" + std::to_string(advertisedCapacity));
            } else {
                add(ProbeStatus::Observation,
                    "NAK Full data threshold",
                    "No F d up to " + std::to_string(maxPoints) +
                        " blank points; largest ACK was " + std::to_string(largestAcked) +
                        "; advertised_capacity=" + std::to_string(advertisedCapacity));
            }
        }
        sendStopToIdle();
    }

    void runEstopProbes() {
        runOneEstopCommand(0x00);
        runOneEstopCommand(0xff);
    }

    void runOneEstopCommand(std::uint8_t command) {
        sendStopToIdle();
        const auto response = sendAndExpect("Emergency stop " + hexByte(command),
                                            singleByteCommand(command),
                                            command);
        expect("Emergency stop " + hexByte(command) + " ACK",
               isAckFor(response, command)
                   && response.response.status.lightEngineState == libera::etherdream::LightEngineState::Estop,
               responseName(response) + " status={" +
                   (response.received ? statusLine(response.response.status) : std::string{}) + "}");

        const auto clear = sendAndExpect("Clear E-stop after " + hexByte(command),
                                         singleByteCommand('c'),
                                         'c');
        const bool ackCleared = isAckFor(clear, 'c')
            && clear.response.status.lightEngineState == libera::etherdream::LightEngineState::Ready;
        const bool stopCondition = clear.received
            && clear.response.response == static_cast<std::uint8_t>('!');
        expect("Clear E-stop after " + hexByte(command),
               ackCleared || stopCondition,
               responseName(clear) + " status={" +
                   (clear.received ? statusLine(clear.response.status) : std::string{}) + "}");
        if (ackCleared) {
            const auto prepare = sendAndExpect("Prepare after E-stop clear " + hexByte(command),
                                               singleByteCommand('p'),
                                               'p');
            expect("Prepare after E-stop clear " + hexByte(command),
                   isAckFor(prepare, 'p')
                       && prepare.response.status.lightEngineState == libera::etherdream::LightEngineState::Ready
                       && prepare.response.status.playbackState == libera::etherdream::PlaybackState::Prepared,
                   responseName(prepare) + " status={" +
                       (prepare.received ? statusLine(prepare.response.status) : std::string{}) + "}");
            expect("Prepare clears E-stop playback flag " + hexByte(command),
                   prepare.received && !prepare.response.status.hasPlaybackEstop(),
                   responseName(prepare) + " status={" +
                       (prepare.received ? statusLine(prepare.response.status) : std::string{}) + "}");
        }
    }

    void runExclusiveConnectionProbe() {
        libera::net::TcpClient second;
        second.setConnectTimeout(350ms);
        std::error_code ec;
        const auto address = libera::net::asio::ip::make_address(options.ip, ec);
        if (ec) {
            add(ProbeStatus::Skipped, "One-host connection exclusivity", "Invalid IP.");
            return;
        }
        const libera::net::tcp::endpoint endpoint(
            address,
            libera::etherdream::config::ETHERDREAM_DAC_PORT_DEFAULT);
        ec = second.connect(endpoint);
        if (ec) {
            add(ProbeStatus::Pass,
                "One-host connection exclusivity",
                "Second TCP connect failed while first was open: " + ec.message());
            return;
        }

        second.setDefaultTimeout(250ms);
        std::array<std::uint8_t, 22> raw{};
        const auto readError = second.read_exact(raw.data(), raw.size(), 250ms);
        second.close();
        expect("One-host connection exclusivity",
               static_cast<bool>(readError),
               "Second TCP connection was accepted; read result=" + readError.message());
    }

    bool connectProbeSession(libera::net::TcpClient& target,
                             std::string& detail,
                             std::chrono::milliseconds timeout = 900ms) {
        std::error_code ec;
        const auto address = libera::net::asio::ip::make_address(options.ip, ec);
        if (ec) {
            detail = "Invalid IP " + options.ip + ": " + ec.message();
            return false;
        }

        target.setConnectTimeout(timeout);
        target.setDefaultTimeout(timeout);
        const libera::net::tcp::endpoint endpoint(
            address,
            libera::etherdream::config::ETHERDREAM_DAC_PORT_DEFAULT);
        ec = target.connect(endpoint);
        if (ec) {
            detail = "connect failed: " + ec.message();
            return false;
        }
        target.setLowLatency();

        const auto initial = readResponseFrom(target, timeout);
        if (!isAckFor(initial, '?')) {
            detail = "connected, but initial status was " + responseName(initial) +
                " error=" + initial.error.message();
            return false;
        }

        detail = "connected; initial status={" + statusLine(initial.response.status) + "}";
        return true;
    }

    bool prepareBadDataSession(libera::net::TcpClient& target,
                               bool prepareFirst,
                               std::string& detail) {
        const auto stop = sendAndReadOn(target, singleByteCommand('s'), 's', 500ms);
        detail += "; stop=" + responseName(stop);

        if (!prepareFirst) {
            return true;
        }

        const auto prepare = sendAndReadOn(target, singleByteCommand('p'), 'p', 500ms);
        detail += "; prepare=" + responseName(prepare);
        return isAckFor(prepare, 'p');
    }

    std::string readBadDataResponses(libera::net::TcpClient& target) {
        std::ostringstream detail;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto response = readResponseFrom(target, 250ms);
            if (attempt) {
                detail << "; ";
            }
            if (!response.received) {
                detail << "read" << attempt << "=none(" << response.error.message() << ")";
                break;
            }
            detail << "read" << attempt << "=" << responseName(response)
                   << " status={" << statusLine(response.response.status) << "}";
        }
        return detail.str();
    }

    bool verifyBadDataRecovery(const std::string& caseName, std::string& detail) {
        std::this_thread::sleep_for(350ms);

        libera::net::TcpClient recovery;
        std::string connectDetail;
        if (!connectProbeSession(recovery, connectDetail, 1200ms)) {
            detail = connectDetail;
            recovery.close();
            return false;
        }

        const auto ping = sendAndReadOn(recovery, singleByteCommand('?'), '?', 700ms);
        const bool recovered = isAckFor(ping, '?');
        detail = connectDetail + "; recovery ping=" + responseName(ping) +
            " status={" + (ping.received ? statusLine(ping.response.status) : std::string{}) + "}";
        sendAndReadOn(recovery, singleByteCommand('s'), 's', 500ms);
        recovery.close();

        if (!recovered) {
            detail += "; possible wedge after " + caseName;
        }
        return recovered;
    }

    void runOneBadDataCase(const BadDataCase& testCase) {
        libera::net::TcpClient session;
        std::string connectDetail;
        if (!connectProbeSession(session, connectDetail)) {
            add(ProbeStatus::Mismatch,
                "Bad data " + testCase.name,
                "Could not open isolated session: " + connectDetail);
            session.close();
            return;
        }

        std::string setupDetail = connectDetail;
        if (!prepareBadDataSession(session, testCase.prepareFirst, setupDetail)) {
            add(ProbeStatus::Skipped,
                "Bad data " + testCase.name,
                "Could not prepare isolated session: " + setupDetail);
            session.close();
            return;
        }

        const auto writeError = session.write_all(testCase.payload.data(),
                                                  testCase.payload.size(),
                                                  500ms);
        std::string badDetail = setupDetail +
            "; wrote=" + std::to_string(testCase.payload.size()) + " byte(s)";
        if (writeError) {
            badDetail += "; write_error=" + writeError.message();
        } else {
            badDetail += "; " + readBadDataResponses(session);
        }

        // Each malformed stream is deliberately isolated. Closing here tests
        // whether the DAC can recover after the host abandons an invalid command.
        session.close();

        std::string recoveryDetail;
        const bool recovered = verifyBadDataRecovery(testCase.name, recoveryDetail);
        add(recovered ? ProbeStatus::Pass : ProbeStatus::Mismatch,
            "Bad data " + testCase.name,
            badDetail + "; recovery={" + recoveryDetail + "}");
    }

    std::vector<BadDataCase> badDataCases() const {
        auto onePointMinusOneByte = dataCommand(1);
        if (!onePointMinusOneByte.empty()) {
            onePointMinusOneByte.pop_back();
        }

        auto zeroDataWithTrailingGarbage = dataCommand(0);
        zeroDataWithTrailingGarbage.push_back(0xaa);

        auto onePointWithTrailingGarbage = dataCommand(1);
        onePointWithTrailingGarbage.push_back(0xaa);

        return {
            {"unknown opcode", {0x7eu}, false},
            {"truncated begin", {'b', 0x00, 0x00}, true},
            {"truncated point-rate", {'q', 0x80}, true},
            {"truncated data header count=1", {'d', 0x01, 0x00}, true},
            {"truncated one-point data", std::move(onePointMinusOneByte), true},
            {"huge data count without payload", {'d', 0xff, 0xff}, true},
            {"zero data with trailing garbage", std::move(zeroDataWithTrailingGarbage), true},
            {"one-point data with trailing garbage", std::move(onePointWithTrailingGarbage), true}
        };
    }

    void runBadDataCrashProbe() {
        add(ProbeStatus::Observation,
            "Bad data crash probe",
            "Running isolated malformed-command cases. If a recovery check fails, the DAC may need a power cycle.");

        for (const auto& testCase : badDataCases()) {
            runOneBadDataCase(testCase);
        }
    }

    void printSummary() const {
        int pass = 0;
        int mismatch = 0;
        int observation = 0;
        int skipped = 0;

        std::puts("\nEther Dream protocol probe results:");
        for (const auto& result : results) {
            const char* label = "OBSERVE";
            switch (result.status) {
                case ProbeStatus::Pass:
                    label = "PASS";
                    ++pass;
                    break;
                case ProbeStatus::Mismatch:
                    label = "MISMATCH";
                    ++mismatch;
                    break;
                case ProbeStatus::Observation:
                    label = "OBSERVE";
                    ++observation;
                    break;
                case ProbeStatus::Skipped:
                    label = "SKIP";
                    ++skipped;
                    break;
            }
            std::printf("%-8s %-38s %s\n",
                        label,
                        result.name.c_str(),
                        result.detail.c_str());
        }

        std::printf("\nSummary: pass=%d mismatch=%d observe=%d skipped=%d\n",
                    pass,
                    mismatch,
                    observation,
                    skipped);
    }

    Options options;
    libera::net::TcpClient client;
    std::vector<BroadcastSample> broadcasts;
    std::vector<ProbeResult> results;
    std::uint16_t bufferCapacity = 0;
    std::uint32_t maxPointRate = 0;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--include-estop") {
            options.includeEstop = true;
        } else if (arg == "--include-exclusive") {
            options.includeExclusive = true;
        } else if (arg == "--stress-rate-queue") {
            options.stressRateQueue = true;
        } else if (arg == "--probe-data-full-threshold") {
            options.probeDataFullThreshold = true;
        } else if (arg == "--bad-data-crash-probe") {
            options.badDataCrashProbe = true;
        } else if (arg == "--max-data-points" && i + 1 < argc) {
            const auto parsed = std::strtoul(argv[++i], nullptr, 10);
            options.maxDataFullThresholdPoints =
                static_cast<std::uint16_t>(std::min<unsigned long>(parsed, 65535ul));
        } else if (arg == "--ip" && i + 1 < argc) {
            options.ip = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
        } else {
            options.ip = arg;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    ProtocolProbe probe(options);
    return probe.run();
}
