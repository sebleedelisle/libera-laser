#include "libera/net/NetService.hpp"
#include "libera/log/Log.hpp"

#include <exception>

namespace libera::net {

namespace {
NetService& static_service() {
    static NetService service;
    return service;
}

void runIoContext(asio::io_context& io) {
    while (!io.stopped()) {
        try {
            io.run();
            break;
        } catch (const std::exception& e) {
            logError("[NetService] uncaught exception in IO thread", e.what());
        } catch (...) {
            logError("[NetService] uncaught unknown exception in IO thread");
        }
    }
}
} // namespace

NetService::NetService()
: io(std::make_shared<asio::io_context>())
, workGuard(asio::make_work_guard(*io))
, thread([this]{ runIoContext(*io); })
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
