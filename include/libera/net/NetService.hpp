#pragma once
#include "libera/net/NetConfig.hpp"
#include <thread>
#include <iostream>
#include <memory>

namespace libera::net {

/**
 * @brief RAII wrapper around `asio::io_context` that runs a dedicated I/O thread.
 *
 * Motivation:
 * - openFrameworks code often blocks on the main thread; here we embrace Asio's
 *   async model by driving a single background loop and posting work to it.
 * - Serialising all socket and timer operations through one executor keeps the
 *   app responsive and avoids racy multi-thread socket access.
 *
 * Implementation highlights:
 * - Holds a shared `asio::io_context` plus an `executor_work_guard` to keep it alive.
 * - A background `std::thread` calls `io.run()` until the guard is released.
 *
 * Lifetime notes:
 * - Destroy network clients before `NetService` so their handlers complete while
 *   the `io_context` is still running.
 * - The destructor releases the work guard, calls `stop()`, and joins the thread.
 *
 * Convenience:
 * - `shared_io_context()` returns the process-wide instance for callers that do not
 *   need a dedicated loop.
 */
class NetService {
public:
    NetService();
    ~NetService();

    NetService(const NetService&) = delete;
    NetService& operator=(const NetService&) = delete;
    NetService(NetService&&) = delete;
    NetService& operator=(NetService&&) = delete;

    std::shared_ptr<asio::io_context> getIO() { return io; }

private:
    std::shared_ptr<asio::io_context> io;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard;
    std::thread thread;
};

NetService& ensureNetService();
std::shared_ptr<asio::io_context> shared_io_context();
asio::io_context& io_context();

} // namespace libera::net
