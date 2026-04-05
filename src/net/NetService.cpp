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
: io(std::make_shared<asio::io_context>())
, workGuard(asio::make_work_guard(*io))
, thread([this]{ io->run(); })
{
    logInfo("Creating NetService object");
}

NetService::~NetService() {
    workGuard.reset();
    io->stop();
    if (thread.joinable()) thread.join();
}

NetService& ensureNetService() {
    return static_service();
}

std::shared_ptr<asio::io_context> shared_io_context() {
    return static_service().getIO();
}

asio::io_context& io_context() {
    return *static_service().getIO();
}

} // namespace libera::net
