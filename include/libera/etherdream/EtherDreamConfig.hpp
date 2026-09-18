#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace libera::etherdream::config {

/**
 * @brief Constants that define EtherDream networking and streaming behaviour.
 *
 * Keeping the values here prevents magic numbers from drifting across
 * translation units and makes it easy to tune the integration in one place.
 */

// Networking ------------------------------------------------------------------
constexpr unsigned short ETHERDREAM_DAC_PORT_DEFAULT = 7765;
constexpr unsigned short ETHERDREAM_DISCOVERY_PORT = 7654;
constexpr std::chrono::seconds ETHERDREAM_DISCOVERY_TIMEOUT{3};


// Streaming behaviour ---------------------------------------------------------

constexpr std::size_t ETHERDREAM_DATA_COMMAND_HEADER_BYTES = 3;
constexpr std::size_t ETHERDREAM_DATA_POINT_BYTES = 18;
constexpr std::size_t ETHERDREAM_SINGLE_SEGMENT_PAYLOAD_BYTES = 1400;
constexpr std::size_t ETHERDREAM_SINGLE_SEGMENT_MAX_PACKET_POINTS =
    (ETHERDREAM_SINGLE_SEGMENT_PAYLOAD_BYTES - ETHERDREAM_DATA_COMMAND_HEADER_BYTES) /
    ETHERDREAM_DATA_POINT_BYTES;
// Normal playback refill trigger. Startup, underrun, and recovery paths may
// request smaller batches; this is not a hard minimum data-command size.
constexpr std::size_t ETHERDREAM_NORMAL_REFILL_MIN_DEFICIT_POINTS = 150;
constexpr std::size_t ETHERDREAM_MAX_PACKET_POINTS = 3640;

constexpr std::chrono::milliseconds ETHERDREAM_MIN_SLEEP{1};
constexpr std::chrono::milliseconds ETHERDREAM_MAX_SLEEP{50};
// Real Ether Dreams can occasionally pause command send/ACK completion for a
// few hundred milliseconds without losing the TCP session. Keep this below the
// old 1s Poco timeout, but above normal LAN jitter so minor stalls do not force
// a full close/reconnect cycle.
constexpr std::chrono::milliseconds ETHERDREAM_COMMAND_TIMEOUT{500};
// Match the legacy fixed reconnect cadence for the first retry, then back off
// if a DAC is genuinely unavailable.
constexpr std::chrono::milliseconds ETHERDREAM_RECONNECT_INITIAL_DELAY{100};
constexpr std::chrono::milliseconds ETHERDREAM_RECONNECT_BACKOFF_DELAY{250};
constexpr std::chrono::milliseconds ETHERDREAM_RECONNECT_MAX_DELAY{1000};
// If the replacement connection survives longer than a single command timeout,
// treat later failures as intermittent rather than continuing an old
// exponential backoff streak.
constexpr std::chrono::seconds ETHERDREAM_RECONNECT_STABLE_RESET_TIME{1};
static_assert(ETHERDREAM_RECONNECT_STABLE_RESET_TIME > ETHERDREAM_COMMAND_TIMEOUT,
              "Reconnect stable reset must exceed the command timeout");

// Newer EtherDream hardware (v3/v4) uses DMA batches internally; keep at least
// this many points buffered to avoid underruns when running at low point rates.
constexpr std::size_t ETHERDREAM_MIN_BUFFER_POINTS = 256;
// Keep meaningful free space in the DAC FIFO. The latency target is still used,
// but it is capped below full so a large app latency cannot force us to fill
// right up to the Ether Dream's limit.
constexpr std::size_t ETHERDREAM_MIN_FREE_POINTS = 512;
constexpr std::size_t ETHERDREAM_SAFETY_HEADROOM_POINTS = ETHERDREAM_MIN_FREE_POINTS;
// Real Ether Dreams can reject writes that would land exactly on the advertised
// FIFO capacity. Keep one point spare for data-packet admission.
constexpr std::size_t ETHERDREAM_WRITE_HEADROOM_POINTS = 1;

// Ether Dream point rates are normally in the tens of thousands of points per
// second. Anything above this is treated as corrupted state rather than a
// usable playback rate.
constexpr std::uint32_t ETHERDREAM_MAX_REASONABLE_POINT_RATE = 1'000'000;

} // namespace libera::etherdream::config
