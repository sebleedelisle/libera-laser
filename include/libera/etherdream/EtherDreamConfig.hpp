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

constexpr std::size_t ETHERDREAM_MIN_PACKET_POINTS = 150;  // minimum batch we want to ship
constexpr std::size_t ETHERDREAM_MAX_PACKET_POINTS = 3640;  // maximum batch we want to ship

constexpr std::chrono::milliseconds ETHERDREAM_MIN_SLEEP{1};
constexpr std::chrono::milliseconds ETHERDREAM_MAX_SLEEP{50};
// Match the legacy fixed reconnect cadence for the first retry, then back off
// if a DAC is genuinely unavailable.
constexpr std::chrono::milliseconds ETHERDREAM_RECONNECT_INITIAL_DELAY{100};
constexpr std::chrono::milliseconds ETHERDREAM_RECONNECT_BACKOFF_DELAY{250};
constexpr std::chrono::milliseconds ETHERDREAM_RECONNECT_MAX_DELAY{1000};
constexpr std::chrono::seconds ETHERDREAM_RECONNECT_STABLE_RESET_TIME{2};

// Newer EtherDream hardware (v3/v4) uses DMA batches internally; keep at least
// this many points buffered to avoid underruns when running at low point rates.
constexpr std::size_t ETHERDREAM_MIN_BUFFER_POINTS = 256;
// Keep meaningful free space in the DAC FIFO. The latency target is still used,
// but it is capped below full so a large app latency cannot force us to fill
// right up to the Ether Dream's limit.
constexpr std::size_t ETHERDREAM_MIN_FREE_POINTS = 512;
constexpr std::size_t ETHERDREAM_SAFETY_HEADROOM_POINTS = ETHERDREAM_MIN_FREE_POINTS;

// Ether Dream point rates are normally in the tens of thousands of points per
// second. Anything above this is treated as corrupted state rather than a
// usable playback rate.
constexpr std::uint32_t ETHERDREAM_MAX_REASONABLE_POINT_RATE = 1'000'000;

} // namespace libera::etherdream::config
