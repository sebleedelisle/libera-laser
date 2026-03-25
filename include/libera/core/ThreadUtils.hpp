#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <string_view>
#include "libera/log/Log.hpp"

namespace libera::core {

/// Join a thread with a timeout, using an atomic flag that the thread sets
/// just before returning. If the thread hasn't finished within the deadline,
/// it is detached to prevent the application from hanging on shutdown.
///
/// @param t        The thread to join.
/// @param finished Atomic flag that the thread sets to true before returning.
/// @param timeout  Maximum time to wait for the thread to finish.
/// @param label    Optional label for the warning log message.
/// @return true if joined successfully, false if timed out and detached.
inline bool timedJoin(std::thread& t,
                      const std::atomic<bool>& finished,
                      std::chrono::milliseconds timeout,
                      std::string_view label = {}) {
    if (!t.joinable()) return true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!finished.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            logWarning("[shutdown] thread did not exit within timeout",
                       label.empty() ? std::string_view{"(unnamed)"} : label,
                       "- detaching to prevent hang");
            t.detach();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Thread has set its finished flag, join returns immediately.
    t.join();
    return true;
}

} // namespace libera::core
