#pragma once
#include "libera/net/NetConfig.hpp"
#include <chrono>
#include "libera/net/Deadline.hpp"

namespace libera::net {

/**
 * UdpSocket
 *
 * Small helper for UDP use-cases like device discovery or broadcast.
 *
 * Notes for openFrameworks users:
 * - UDP in Asio is also async; here we provide `send_to` / `recv_from` that use
 *   the same `with_deadline` pattern as TCP to provide timeouts.
 * - Enable broadcast on macOS/Linux by setting the socket option when needed.
 */
class UdpSocket {
public:
    explicit UdpSocket(asio::io_context& io) : sock_(io) {}

    std::error_code open_v4() {
        std::error_code ec;
        sock_.open(udp::v4(), ec);
        if (ec) {
            logError("[UdpSocket] open_v4 failed: ", ec.message(), "\n");
        }
        return ec;
    }

    std::error_code bind_any(uint16_t port) {
        std::error_code ec;
        sock_.bind(udp::endpoint(udp::v4(), port), ec);
        if (ec) {
            logError("[UdpSocket] bind_any failed on port ", port, ": ", ec.message(), "\n");
        }
        return ec;
    }

    std::error_code enable_broadcast(bool on=true) {
        std::error_code ec;
        sock_.set_option(asio::socket_base::broadcast(on), ec);
        return ec;
    }

    // Send a datagram, fail if not sent within timeout.
    std::error_code send_to(const void* data, std::size_t n,
                                      const udp::endpoint& ep, std::chrono::milliseconds timeout) {
        auto ex = sock_.get_executor();
        return with_deadline(ex, timeout,
            [&](auto cb){ sock_.async_send_to(asio::buffer(data, n), ep, 0, cb); },
            [&]{ sock_.cancel(); });
    }

    // Receive one datagram, with timeout. Returns ec + fills out_ep + out_n.
    std::error_code recv_from(void* data, std::size_t max,
                                        udp::endpoint& out_ep, std::size_t& out_n,
                                        std::chrono::milliseconds timeout) {
        auto ex = sock_.get_executor();
        out_n = 0;
        return with_deadline(ex, timeout,
            [&](auto cb){
                sock_.async_receive_from(asio::buffer(data, max), out_ep, 0,
                    [&, cb](const std::error_code& ec, std::size_t n){
                        out_n = n; cb(ec);
                    });
            },
            [&]{ sock_.cancel(); });
    }

    udp::socket& raw() { return sock_; }
    void close() { std::error_code ignore; sock_.close(ignore); }

private:
    udp::socket sock_;
};

} // namespace libera::net
