#pragma once
#include "libera/net/NetConfig.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <memory>
#include <system_error>

/**
 * @brief Run an async operation with a deadline enforced by an Asio timer.
 *
 * Pattern:
 * - Start an async operation and an `asio::steady_timer` on the same executor.
 * - Whichever completes first cancels the other and signals a condition
 *   variable so this call can return synchronously with a timeout.
 *
 * Why it is useful:
 * - Blocking APIs with timeouts are common in openFrameworks; in Asio the
 *   equivalent pattern is "async operation + timer + cancel". This helper wraps
 *   that flow and surfaces the resulting `std::error_code`.
 *
 * Safety notes:
 * - Completion handlers capture a `shared_ptr<State>` so they cannot access
 *   destroyed synchronisation primitives even if they run after this function
 *   returns. This avoids a common use-after-free race in naive implementations.
 * - The `cancel()` functor must cancel the same socket or timer that launched
 *   the operation. It is invoked behind a catch boundary so a cancellation
 *   failure cannot escape the Asio handler thread.
 *
 * Requirements:
 * - The associated `asio::io_context` must already be running while we block.
 *   Otherwise the wait would never complete.
 */
namespace libera::net {

template<typename StartAsync, typename Cancel>
std::error_code with_deadline(
    asio::any_io_executor ex,
    std::chrono::milliseconds timeout,
    StartAsync start_async,
    Cancel cancel,
    const char* label = "",
    bool logTimeout = false)
{
    //std::cout << "[with_deadline] start timeout=" << timeout.count() << "ms\n";
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::error_code ec = asio::error::would_block;
    };

    auto st = std::make_shared<State>();
    auto timer = std::make_shared<asio::steady_timer>(ex);

    // Completion of the user async op
    auto op_handler = [st, timer](const std::error_code& op_ec, auto&&... /*ignored*/) {
        {
            std::lock_guard<std::mutex> lk(st->m);
            if (st->done) return;           // another path already won
            st->ec = op_ec;
            st->done = true;
        }
        st->cv.notify_one();                // wake waiter first...
        try {
            timer->cancel();                // ...then cancel timer behind a catch boundary.
        } catch (const std::exception& e) {
            logError("[with_deadline] timer cancel failed", e.what());
        } catch (...) {
            logError("[with_deadline] timer cancel failed", "unknown exception");
        }
    };

    // Kick off the async operation (it must call our op_handler).
    try {
        start_async(op_handler);
    } catch (const std::system_error& e) {
        logError("[with_deadline] async start failed", label, e.what());
        return e.code();
    } catch (const std::exception& e) {
        logError("[with_deadline] async start failed", label, e.what());
        return asio::error::operation_aborted;
    } catch (...) {
        logError("[with_deadline] async start failed", label, "unknown exception");
        return asio::error::operation_aborted;
    }

    // Arm the deadline
    timer->expires_after(timeout);
    timer->async_wait([st, cancel, timer, timeout, label, logTimeout](const std::error_code& tec){
        if (tec == asio::error::operation_aborted) {
            // Cancelled because the operation finished first, so nothing to do.
            return;
        }
        bool notify = false;
        {
            std::lock_guard<std::mutex> lk(st->m);
            if (st->done) {
                return; // operation already completed; no need to cancel
            }
            st->ec = asio::error::timed_out;
            st->done = true;
            notify = true;
        }
        if (notify) {
            if (logTimeout) {
                logInfo("[with_deadline] timeout fired after", timeout.count(), "ms", label);
            }
            try {
                cancel();
            } catch (const std::exception& e) {
                logError("[with_deadline] cancel failed", label, e.what());
            } catch (...) {
                logError("[with_deadline] cancel failed", label, "unknown exception");
            }
            st->cv.notify_one();
        }
    });

    // Wait until either branch completes
    std::unique_lock<std::mutex> lk(st->m);
    st->cv.wait(lk, [&]{ return st->done; });
    return st->ec;
}

}
