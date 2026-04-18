#pragma once

#include "libera/core/ControllerManagerBase.hpp"
#include "libera/plugin/PluginController.hpp"
#include "libera/plugin/PluginControllerInfo.hpp"

#include <memory>
#include <string>
#include <vector>

namespace libera::plugin {

/*
 * Represents one loaded plugin shared library and its optional backend state.
 */
struct LoadedPlugin {
    void* libraryHandle = nullptr;
    const libera_plugin_api_t* api = nullptr;
    void* backendHandle = nullptr;
    std::string typeName;
    std::string displayName;
    std::string path;
    bool initialised = false;
};

/*
 * A ControllerManagerBase implementation that delegates to a single loaded
 * plugin.  One PluginDelegateManager is created per successfully loaded plugin
 * library and registered with the System.
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

    ControllerPtr createController(const PluginControllerInfo& info) override;
    NewControllerDisposition prepareNewController(PluginController& controller,
                                                  const PluginControllerInfo& info) override;
    void afterCloseControllers() override;
};

/*
 * Load all plugin shared libraries from a directory and register a
 * PluginDelegateManager for each one via AddControllerManager().
 *
 * Call this once at startup before constructing libera::System.
 */
void loadPluginsFromDirectory(const std::string& path);

} // namespace libera::plugin
