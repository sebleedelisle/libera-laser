#pragma once

#include "libera/core/LaserController.hpp"
#include "libera/plugin/libera_plugin.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace libera::plugin {

/*
 * Function-pointer table resolved from a loaded plugin shared library.
 * Stored once per plugin and shared across all controllers from the same plugin.
 */
struct PluginFunctions {
    decltype(&libera_plugin_api_version)       api_version       = nullptr;
    decltype(&libera_plugin_type)              type              = nullptr;
    decltype(&libera_plugin_name)              name              = nullptr;
    decltype(&libera_plugin_init)              init              = nullptr;
    decltype(&libera_plugin_shutdown)          shutdown          = nullptr;
    decltype(&libera_plugin_discover)          discover          = nullptr;
    decltype(&libera_plugin_connect)           connect           = nullptr;
    decltype(&libera_plugin_send_points)       send_points       = nullptr;
    decltype(&libera_plugin_get_buffer_fullness) get_buffer_fullness = nullptr;
    decltype(&libera_plugin_set_armed)         set_armed         = nullptr;
    decltype(&libera_plugin_disconnect)        disconnect        = nullptr;
};

/*
 * Wraps a single plugin-managed DAC connection as a libera LaserController.
 *
 * The worker thread (run()) pulls frames from the frame queue, converts points
 * to the plugin's wire format, and calls into the plugin's send_points function.
 */
class PluginController : public core::LaserController {
public:
    PluginController(const PluginFunctions& funcs, const std::string& controllerId);
    ~PluginController() override;

    bool open();

private:
    void run() override;

    const PluginFunctions& funcs;
    std::string controllerId;
    void* pluginHandle = nullptr;
    std::atomic<bool> connected{false};
};

} // namespace libera::plugin
