#pragma once

#include "libera/core/LaserController.hpp"
#include "libera/plugin/libera_plugin.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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
 * The worker thread (run()) stays host-owned: it pulls either point batches or
 * whole frames from Libera's shared content-source pipeline, converts them to
 * the plugin wire format, and forwards them through the plugin callbacks.
 */
class PluginController : public core::LaserController,
                         public std::enable_shared_from_this<PluginController> {
public:
    PluginController(const libera_plugin_api_t* api,
                     void* backendHandle,
                     const libera_controller_info_t& controllerInfo,
                     std::string pluginPath = {});
    ~PluginController() override;

    bool open();
    void close();

    void setPointRate(std::uint32_t pointRateValue) override;

    // Settings use the stable discovery identity for persistence and the
    // opaque live handle only while applying a value to this connection.
    const std::string& pluginType() const { return pluginTypeName; }
    std::string controllerId() const { return controllerInfo.id; }
    libera_status_t applySetting(const std::string& key,
                                 const std::string& value);

    // Called by the host-services callbacks installed in PluginManager.
    void recordLatencyFromPlugin(std::uint64_t nanoseconds);
    void reportErrorFromPlugin(const char* code, const char* label);

    // Device property accessors — return empty if the plugin does not expose
    // static properties or a reader callback.
    std::vector<PluginProperty> listProperties() const;
    std::optional<std::string>  getProperty(const std::string& key) const;

private:
    void run() override;
    bool usesFrameTransport() const;
    bool updateBufferTelemetry(std::uint32_t rate,
                               libera_buffer_state_t& bufferState,
                               bool clearOnMissingTelemetry = true);
    void handlePluginFailure(libera_status_t status,
                             const char* action,
                             const char* errorPrefix);

    const libera_plugin_api_t* api = nullptr;
    void* backendHandle = nullptr;
    libera_controller_info_t controllerInfo{};
    std::string pluginPath;
    std::string pluginTypeName;
    void* pluginHandle = nullptr;
    std::atomic<bool> connected{false};

    // Plugins own the opaque controller handle, so every callback using it is
    // serialized with live setting changes. The mutex is recursive because a
    // reconnect restores saved settings through the same public setting path.
    mutable std::recursive_mutex pluginCallMutex;

    // Last armed state pushed to the plugin via api->set_armed(). Only touched
    // from the run() thread; we push on transition rather than on every tick.
    bool lastSentArmed = false;

    // Host-side emitted-point counter. This keeps the plugin adapter aligned
    // with built-in backends that pass a monotonic point index into the shared
    // scheduling helpers.
    std::uint64_t currentPointIndex = 0;

    // Smoothed estimate of how long api->send_frame() takes to push a frame to
    // the plugin. Feeds projectedNextWriteRenderTime() so the FrameScheduler's
    // due-time gate uses the real play-time of the next-to-write frame, not
    // an instant-now assumption.
    std::int64_t smoothedSendFrameMicros = 0;
};

} // namespace libera::plugin
