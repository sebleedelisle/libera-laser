#pragma once
#include "libera/net/NetConfig.hpp"
#include "libera/net/Deadline.hpp"
#include "libera/net/TimeoutConfig.hpp"
#include "libera/net/NetService.hpp"
#include "libera/log/Log.hpp"

#include <chrono>
#include <memory>

namespace libera::net {
using duration = TimeoutConfig::duration;

/**
 * @brief Thin wrapper around `tcp::socket` that adds deadlines and low-latency options.
 *
 * Highlights:
 * - `connect(...)` retries endpoints and respects a per-attempt timeout.
 * - `read_exact(...)` and `write_all(...)` block the caller while enforcing deadlines.
 * - `setLowLatency()` enables TCP_NODELAY and keepalive to reduce jitter.
 * - All socket work is serialized by a strand executor.
 *
 * The caller must keep the owning `asio::io_context` running while using the API.
 */
class TcpClient {
public:
    TcpClient()
    : io(shared_io_context())
    , socket(*io)
    , strand(asio::make_strand(*io))
    , defaultTimeout(TimeoutConfig::defaultTimeout())
    , connectTimeout(TimeoutConfig::defaultTimeout())
    {}

    void setDefaultTimeout(duration timeout) {
        defaultTimeout = sanitize(timeout);
    }

    duration getDefaultTimeout() const { return defaultTimeout; }

    void setConnectTimeout(duration timeout) {
        connectTimeout = sanitize(timeout);
    }

    duration getConnectTimeout() const { return connectTimeout; }

    // Access to the socket (non-const)
    tcp::socket& getSocket() { return socket; }

    // Overload 1: connect from a range/container of *endpoints* (e.g., std::array<tcp::endpoint, N>)
    // Delegates to the single-endpoint overload so each attempt resets the socket
    // before calling connect_one().
    std::error_code connect(const tcp::endpoint& endpoint, duration timeout) {
        close();
        socket = tcp::socket(strand);
        auto ec = connect_one(endpoint, timeout);
        if (ec) {
            // A timed-out async_connect leaves the socket open but unusable until it
            // is explicitly closed. Clear that stale state before the caller retries.
            close();
        }
        return ec;
    }

    std::error_code connect(const tcp::endpoint& endpoint) {
        return connect(endpoint, connectTimeout);
    }

    template <typename Endpoints>
    std::error_code connect(const Endpoints& endpoints, duration timeout) {
        std::error_code last = asio::error::host_not_found;

        for (const auto& e : endpoints) {
            auto ec = connect(endpoint_of(e), timeout);
            if (!ec) return ec;   // success
            last = ec;            // remember last error and try next
        }
        return last;
    }

    template <typename Endpoints>
    std::error_code connect(const Endpoints& endpoints) {
        return connect(endpoints, connectTimeout);
    }

    // Overload 2: connect from resolver results (entries have .endpoint())
    // SFINAE ensures we pick this when Results::value_type has endpoint().
    template <typename Results>
    std::error_code connect(Results results, duration timeout,
                       // SFINAE: prefer this when value_type has endpoint()
                       decltype(std::declval<typename Results::value_type>().endpoint(), 0) = 0) {
        std::error_code last = asio::error::host_not_found;

        for (auto& e : results) {
            auto ec = connect(e.endpoint(), timeout);
            if (!ec) return ec;
            last = ec;
        }
        return last;
    }

    template <typename Results>
    std::error_code connect(Results results,
                       decltype(std::declval<typename Results::value_type>().endpoint(), 0) = 0) {
        return connect(results, connectTimeout);
    }

    std::error_code read_exact(void* buf, std::size_t n, duration timeout,
                               std::size_t* bytesTransferredOut = nullptr) {
        auto ex = socket.get_executor();
        const auto effectiveTimeout = sanitize(timeout);
        // Keep the byte count alive even if the handler fires after this call
        // returns (e.g. cancellation races). A stack variable would be invalid
        // by then and trips ASan/Guard Malloc.
        auto bytesTransferredPtr = std::make_shared<std::size_t>(0);
        auto ec = with_deadline(ex, effectiveTimeout,
            [&](auto completion){
                asio::async_read(socket, asio::buffer(buf, n),
                    [&, completion, bytesTransferredPtr](const std::error_code& op_ec, std::size_t transferred){
                        *bytesTransferredPtr = transferred;
                        completion(op_ec);
                    });
            },
            [&]{ socket.cancel(); },
            "tcp_read"
        );
        if (bytesTransferredOut) {
            *bytesTransferredOut = *bytesTransferredPtr;
        }
        return ec;
    }

    std::error_code write_all(const void* buf, std::size_t n, duration timeout) {
        auto ex = socket.get_executor();
        const auto effectiveTimeout = sanitize(timeout);
        auto ec = with_deadline(ex, effectiveTimeout,
            [&](auto completion){
                asio::async_write(socket, asio::buffer(buf, n),
                    [completion](const std::error_code& op_ec, std::size_t){
                        completion(op_ec);
                    });
            },
            [&]{ socket.cancel(); },
            "tcp_write"
        );
        return ec;
    }

    std::error_code read_exact(void* buf, std::size_t n, std::size_t* bytesTransferredOut = nullptr) {
        return read_exact(buf, n, defaultTimeout, bytesTransferredOut);
    }

    std::error_code write_all(const void* buf, std::size_t n) {
        return write_all(buf, n, defaultTimeout);
    }

    void setLowLatency() {
        std::error_code ec;
        socket.set_option(tcp::no_delay(true), ec);
        socket.set_option(asio::socket_base::keep_alive(true), ec);
    }

    bool is_open() const { return socket.is_open(); }
    bool is_connected() const {
        if (!socket.is_open()) {
            return false;
        }
        std::error_code ec;
        (void)socket.remote_endpoint(ec);
        return !ec;
    }

    // Best-effort cancellation of pending ops on the socket.
    // Useful during shutdown to nudge operations to complete now rather than
    // waiting for timeouts.
    void cancel() {
        std::error_code ec;
        socket.cancel(ec);
    }

    void close() {
        logInfo("[TcpClient] close()");
        if (!socket.is_open()) return;
        std::error_code ec;
        // Proactively cancel outstanding operations first (pattern: cancel -> shutdown -> close).
        socket.cancel(ec);
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

private:
    // Helper to accept either a tcp::endpoint or a resolver result entry.
    static tcp::endpoint endpoint_of(const tcp::endpoint& ep) { return ep; }

    template <typename Entry>
    static auto endpoint_of(const Entry& e) -> decltype(e.endpoint()) {
        return e.endpoint();
    }

    std::error_code connect_one(const tcp::endpoint& ep, duration timeout) {
        auto ex = socket.get_executor();
        const auto effectiveTimeout = sanitize(timeout);
        return with_deadline(ex, effectiveTimeout,
            [&](auto completion){ socket.async_connect(ep, completion); },
            [&]{ socket.cancel(); },
            "tcp_connect"
        );
    }

    static duration sanitize(duration timeout) {
        return timeout.count() < 0 ? duration::zero() : timeout;
    }

    std::shared_ptr<asio::io_context> io;
    tcp::socket socket;
    asio::strand<asio::io_context::executor_type> strand;
    duration defaultTimeout;
    duration connectTimeout;
};

} // namespace libera::net
