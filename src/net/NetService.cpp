#include "libera/net/NetService.hpp"
#include "libera/log/Log.hpp"

namespace libera::net {

namespace {
NetService& static_service() {
    static NetService service;
    return service;
}
} // namespace

NetService::NetService()
: io_(std::make_shared<asio::io_context>())
, work_guard_(asio::make_work_guard(*io_))
, t_([this]{ io_->run(); })
{
    logInfo("Creating NetService object");
}

NetService::~NetService() {
    work_guard_.reset();
    io_->stop();
    if (t_.joinable()) t_.join();
}

NetService& ensureNetService() {
    return static_service();
}

std::shared_ptr<asio::io_context> shared_io_context() {
    return static_service().io();
}

asio::io_context& io_context() {
    return *static_service().io();
}

} // namespace libera::net
