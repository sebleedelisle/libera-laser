#pragma once

#include <chrono>
#include <cstddef>

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

constexpr std::chrono::milliseconds ETHERDREAM_MIN_BUFFER_MS{50};
// Newer EtherDream hardware (v3/v4) uses DMA batches internally; keep at least
// this many points buffered to avoid underruns when running at low point rates.
constexpr std::size_t ETHERDREAM_MIN_BUFFER_POINTS = 256;


} // namespace libera::etherdream::config
