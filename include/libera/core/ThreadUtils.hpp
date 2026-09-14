#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <string_view>
#include "libera/log/Log.hpp"

namespace libera::core {

/// Join a thread, logging if it takes longer than the requested timeout.
///
/// These threads usually capture their owning object. Detaching on timeout
/// lets the owner continue destruction while the thread can still touch `this`,
/// so we keep waiting after the warning and only return once the join is done.
///
/// @param t        The thread to join.
/// @param finished Atomic flag that the thread sets to true before returning.
/// @param timeout  Maximum time to wait for the thread to finish.
/// @param label    Optional label for the warning log message.
/// @return true if the thread joined within the timeout, false if it joined
/// after the timeout warning.
inline bool timedJoin(std::thread& t,
                      const std::atomic<bool>& finished,
                      std::chrono::milliseconds timeout,
                      std::string_view label = {}) {
    if (!t.joinable()) return true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool joinedWithinTimeout = true;
    while (!finished.load(std::memory_order_acquire)) {
        if (joinedWithinTimeout && std::chrono::steady_clock::now() >= deadline) {
            joinedWithinTimeout = false;
            logInfo("[shutdown] thread did not exit within timeout ",
                    (label.empty() ? std::string_view{"(unnamed)"} : label),
                    " - waiting to avoid detached owner access");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Thread has set its finished flag, join returns immediately.
    t.join();
    return joinedWithinTimeout;
}

} // namespace libera::core
