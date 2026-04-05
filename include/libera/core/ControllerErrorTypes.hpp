#pragma once

#include <string_view>

namespace libera::core::error_types {

namespace network {
inline constexpr std::string_view connectFailed = "network.connect_failed";
inline constexpr std::string_view connectionLost = "network.connection_lost";
inline constexpr std::string_view sendFailed = "network.send_failed";
inline constexpr std::string_view receiveFailed = "network.receive_failed";
inline constexpr std::string_view timeout = "network.timeout";
inline constexpr std::string_view protocolError = "network.protocol_error";
inline constexpr std::string_view packetLoss = "network.packet_loss";
inline constexpr std::string_view bufferUnderflow = "network.buffer_underflow";
inline constexpr std::string_view bufferOverrun = "network.buffer_overrun";
} // namespace network

namespace usb {
inline constexpr std::string_view connectFailed = "usb.connect_failed";
inline constexpr std::string_view connectionLost = "usb.connection_lost";
inline constexpr std::string_view transferFailed = "usb.transfer_failed";
inline constexpr std::string_view timeout = "usb.timeout";
inline constexpr std::string_view statusError = "usb.status_error";
} // namespace usb

namespace plugin {
inline constexpr std::string_view sendDisconnected = "plugin.send.disconnected";
inline constexpr std::string_view sendTimeout      = "plugin.send.timeout";
inline constexpr std::string_view sendBusy         = "plugin.send.busy";
inline constexpr std::string_view sendProtocol     = "plugin.send.protocol";
inline constexpr std::string_view sendInvalidArg   = "plugin.send.invalid_argument";
inline constexpr std::string_view sendInternal     = "plugin.send.internal";
inline constexpr std::string_view sendUnknown      = "plugin.send.unknown";
inline constexpr std::string_view error            = "plugin.error";
} // namespace plugin

inline constexpr std::string_view labelFor(std::string_view code) {
    if (code == network::connectFailed) return "Network connect failed";
    if (code == network::connectionLost) return "Network connection lost";
    if (code == network::sendFailed) return "Network send failed";
    if (code == network::receiveFailed) return "Network receive failed";
    if (code == network::timeout) return "Network timeout";
    if (code == network::protocolError) return "Network protocol error";
    if (code == network::packetLoss) return "Network packet loss";
    if (code == network::bufferUnderflow) return "Network buffer underflow";
    if (code == network::bufferOverrun) return "Network buffer overrun";

    if (code == usb::connectFailed) return "USB connect failed";
    if (code == usb::connectionLost) return "USB connection lost";
    if (code == usb::transferFailed) return "USB transfer failed";
    if (code == usb::timeout) return "USB timeout";
    if (code == usb::statusError) return "USB status error";

    if (code == plugin::sendDisconnected) return "Plugin disconnected";
    if (code == plugin::sendTimeout)      return "Plugin send timeout";
    if (code == plugin::sendBusy)         return "Plugin busy";
    if (code == plugin::sendProtocol)     return "Plugin protocol error";
    if (code == plugin::sendInvalidArg)   return "Plugin invalid argument";
    if (code == plugin::sendInternal)     return "Plugin internal error";
    if (code == plugin::sendUnknown)      return "Plugin send failed";
    if (code == plugin::error)            return "Plugin error";

    // Plugin-reported codes we don't recognise: show the code itself rather
    // than "Unknown error" so the user has something actionable to grep for.
    if (code.size() > 7 && code.substr(0, 7) == "plugin.") return code;
    // Likewise for domain-prefixed plugin codes (e.g. "shownet.offline").
    if (code.find('.') != std::string_view::npos) return code;

    return "Unknown error";
}

} // namespace libera::core::error_types
