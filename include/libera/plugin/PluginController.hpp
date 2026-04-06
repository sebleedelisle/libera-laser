#pragma once

#include "libera/core/LaserController.hpp"
#include "libera/plugin/libera_plugin.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    decltype(&libera_plugin_set_point_rate)    set_point_rate    = nullptr;
    decltype(&libera_plugin_send_points)       send_points       = nullptr;
    decltype(&libera_plugin_get_buffer_state)  get_buffer_state  = nullptr;
    decltype(&libera_plugin_set_armed)         set_armed         = nullptr;
    decltype(&libera_plugin_disconnect)        disconnect        = nullptr;

    // Optional — may be null if the plugin does not export them.
    decltype(&libera_plugin_rescan)            rescan            = nullptr;
    decltype(&libera_plugin_list_properties)   list_properties   = nullptr;
    decltype(&libera_plugin_get_property)      get_property      = nullptr;
};

struct PluginProperty {
    std::string key;
    std::string label;
};

/*
 * Wraps a single plugin-managed DAC connection as a libera LaserController.
 *
 * The worker thread (run()) drives a streaming loop: request points from the
 * host's frame-mode callback, convert to the plugin's wire format, and call
 * into send_points.  Back-pressure comes from the plugin's reported buffer
 * state (if available).
 */
class PluginController : public core::LaserController {
public:
    PluginController(const PluginFunctions& funcs, const std::string& controllerId);
    ~PluginController() override;

    bool open();

    void setPointRate(std::uint32_t pointRateValue) override;

    // Called by the host-services callbacks installed in PluginManager.
    void recordLatencyFromPlugin(std::uint64_t nanoseconds);
    void reportErrorFromPlugin(const char* code, const char* label);

    // Device property accessors — return empty if the plugin does not
    // expose the corresponding optional exports.
    std::vector<PluginProperty> listProperties() const;
    std::optional<std::string>  getProperty(const std::string& key) const;

private:
    void run() override;

    const PluginFunctions& funcs;
    std::string controllerId;
    void* pluginHandle = nullptr;
    std::atomic<bool> connected{false};

    // Last armed state pushed to the plugin via funcs.set_armed(). Only
    // touched from the run() thread; we push on transition rather than
    // on every tick.
    bool lastSentArmed = false;
};

} // namespace libera::plugin
