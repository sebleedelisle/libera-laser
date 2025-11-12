#include "libera/log/Log.hpp"

#include <iostream>
#include <mutex>
#include <string>

namespace libera::log {

namespace {

LogHandler makeDefaultInfoSink() {
    return [](std::string_view message) {
        std::cout << message;
        std::cout.flush();
    };
}

LogHandler makeDefaultErrorSink() {
    return [](std::string_view message) {
        std::cerr << message;
        std::cerr.flush();
    };
}

std::mutex sinkMutex;
LogHandler infoHandler = makeDefaultInfoSink();
LogHandler errorHandler = makeDefaultErrorSink();

} // namespace

void setInfoLogHandler(LogHandler handler) {
    std::lock_guard lock(sinkMutex);
    infoHandler = handler ? std::move(handler) : makeDefaultInfoSink();
}

void setErrorLogHandler(LogHandler handler) {
    std::lock_guard lock(sinkMutex);
    errorHandler = handler ? std::move(handler) : makeDefaultErrorSink();
}

void setLogHandlers(LogHandler newInfo, LogHandler newError) {
    std::lock_guard lock(sinkMutex);
    infoHandler = newInfo ? std::move(newInfo) : makeDefaultInfoSink();
    errorHandler = newError ? std::move(newError) : makeDefaultErrorSink();
}

void resetLogHandlers() {
    std::lock_guard lock(sinkMutex);
    infoHandler = makeDefaultInfoSink();
    errorHandler = makeDefaultErrorSink();
}

void logInfo(std::string_view message) {
    LogHandler handler;
    {
        std::lock_guard lock(sinkMutex);
        handler = infoHandler;
    }
    if (handler) {
        std::string line{message};
        line.push_back('\n');
        handler(line);
    }
}

void logError(std::string_view message) {
    LogHandler handler;
    {
        std::lock_guard lock(sinkMutex);
        handler = errorHandler;
    }
    if (handler) {
        std::string line{message};
        line.push_back('\n');
        handler(line);
    }
}

} // namespace libera::log
