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

struct PluginProperty {
    std::string key;
    std::string label;
};

/*
 * Wraps one plugin-managed controller connection as a Libera LaserController.
 *
 * The worker thread (run()) stays host-owned: it pulls points from Libera's
 * shared content-source pipeline, converts them to the plugin wire format, and
 * forwards them through the plugin's controller callbacks.
 */
class PluginController : public core::LaserController {
public:
    PluginController(const libera_plugin_api_t* api,
                     void* backendHandle,
                     const libera_controller_info_t& controllerInfo);
    ~PluginController() override;

    bool open();

    void setPointRate(std::uint32_t pointRateValue) override;

    // Called by the host-services callbacks installed in PluginManager.
    void recordLatencyFromPlugin(std::uint64_t nanoseconds);
    void reportErrorFromPlugin(const char* code, const char* label);

    // Device property accessors — return empty if the plugin does not expose
    // static properties or a reader callback.
    std::vector<PluginProperty> listProperties() const;
    std::optional<std::string>  getProperty(const std::string& key) const;

private:
    void run() override;

    const libera_plugin_api_t* api = nullptr;
    void* backendHandle = nullptr;
    libera_controller_info_t controllerInfo{};
    void* pluginHandle = nullptr;
    std::atomic<bool> connected{false};

    // Last armed state pushed to the plugin via api->set_armed(). Only touched
    // from the run() thread; we push on transition rather than on every tick.
    bool lastSentArmed = false;
};

} // namespace libera::plugin
