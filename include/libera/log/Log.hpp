#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <sstream>
#include <type_traits>
#include <utility>

namespace libera::log {

using LogHandler = std::function<void(std::string_view)>;

void setInfoLogHandler(LogHandler handler);
void setErrorLogHandler(LogHandler handler);
void setLogHandlers(LogHandler infoHandler, LogHandler errorHandler);
void resetLogHandlers();

void logInfo(std::string_view message);
void logError(std::string_view message);

namespace detail {

template<typename... Args>
inline std::string buildLogMessage(Args&&... args) {
    std::ostringstream oss;
    bool first = true;
    auto append = [&](auto&& value) {
        if (!first) {
            oss << ", ";
        } else {
            first = false;
        }
        oss << std::forward<decltype(value)>(value);
    };
    (append(std::forward<Args>(args)), ...);
    return oss.str();
}

template<typename T>
using IsStringViewConvertible = std::is_convertible<T, std::string_view>;

} // namespace detail

template<typename First, typename... Rest,
         typename = std::enable_if_t<(sizeof...(Rest) > 0) ||
             !detail::IsStringViewConvertible<std::decay_t<First>>::value>>
void logInfo(First&& first, Rest&&... rest) {
    auto msg = detail::buildLogMessage(std::forward<First>(first), std::forward<Rest>(rest)...);
    logInfo(msg);
}

template<typename First, typename... Rest,
         typename = std::enable_if_t<(sizeof...(Rest) > 0) ||
             !detail::IsStringViewConvertible<std::decay_t<First>>::value>>
void logError(First&& first, Rest&&... rest) {
    auto msg = detail::buildLogMessage(std::forward<First>(first), std::forward<Rest>(rest)...);
    logError(msg);
}


} // namespace libera::log

namespace libera {
using log::LogHandler;
using log::setInfoLogHandler;
using log::setErrorLogHandler;
using log::setLogHandlers;
using log::resetLogHandlers;
using log::logInfo;
using log::logError;
} // namespace libera
