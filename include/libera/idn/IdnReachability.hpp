#pragma once

namespace libera::idn {
// A single missed packet must not disconnect an otherwise healthy output.
inline constexpr unsigned reachabilityFailureThreshold = 3;

inline bool endpointReachable(unsigned missedDiscoveries, int failedPings,
                              bool pingVerified, bool discoveryStale) {
    if (missedDiscoveries >= reachabilityFailureThreshold) return false;
    // Some receivers do not implement ping. Require stale discovery before
    // treating unanswered, never-verified pings as evidence of disconnection.
    return failedPings < static_cast<int>(reachabilityFailureThreshold) ||
           (!pingVerified && !discoveryStale);
}
}
