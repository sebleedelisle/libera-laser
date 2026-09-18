#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/plugin/PluginController.hpp"
#include "libera/plugin/PluginControllerInfo.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace libera::plugin {

/*
 * Represents one loaded native entrypoint and its optional backend state.
 */
struct LoadedPlugin {
    void* libraryHandle = nullptr;
    const libera_plugin_api_t* api = nullptr;
    void* backendHandle = nullptr;
    std::string pluginId;
    std::string version;
    std::string controllerType;
    std::string displayName;
    std::string vendor;
    std::string description;
    std::string packageRoot;
    std::string entrypointPath;
    std::string packageSha256;
    std::optional<std::string> readmePath;
    std::optional<std::string> licensePath;
    bool initialised = false;
    // Serializes backend creation, destruction, discovery, and live
    // plugin-setting changes without blocking independent controller streams.
    std::mutex lifecycleMutex;
};

/*
 * A ControllerManagerBase implementation that delegates to a single loaded
 * plugin. One PluginDelegateManager is created per successfully loaded package
 * revision and registered with the System.
 */
class PluginDelegateManager
    : public core::ControllerManagerBase<PluginControllerInfo,
                                         PluginController> {
public:
    explicit PluginDelegateManager(std::shared_ptr<LoadedPlugin> plugin);
    ~PluginDelegateManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;

private:
    std::shared_ptr<LoadedPlugin> plugin;

    bool ensureBackend();
    ControllerPtr createController(const PluginControllerInfo& info) override;
    NewControllerDisposition prepareNewController(PluginController& controller,
                                                  const PluginControllerInfo& info) override;
    void closeController(const std::string& key,
                         PluginController& controller) override;
    void afterCloseControllers() override;
};

/*
 * Load active package revisions from a plugin store and register a delegate
 * manager for each one via AddControllerManager().
 *
 * Call this once at startup before constructing libera::System.
 */
void loadPluginsFromDirectory(const std::string& path);

} // namespace libera::plugin
