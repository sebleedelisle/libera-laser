#pragma once

#include "libera/System.hpp"
#include "libera/core/ControllerCache.hpp"
#include "libera/plugin/PluginController.hpp"

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
    bool initialised = false;
};

/*
 * A ControllerManagerBase implementation that delegates to a single loaded
 * plugin.  One PluginDelegateManager is created per successfully loaded plugin
 * library and registered with the System.
 */
class PluginDelegateManager : public core::ControllerManagerBase {
public:
    explicit PluginDelegateManager(std::shared_ptr<LoadedPlugin> plugin);
    ~PluginDelegateManager() override;

    std::vector<std::unique_ptr<core::ControllerInfo>> discover() override;
    std::string_view managedType() const override;
    std::shared_ptr<core::LaserController> connectController(const core::ControllerInfo& info) override;
    void closeAll() override;

private:
    std::shared_ptr<LoadedPlugin> plugin;
    core::ControllerCache<std::string, PluginController> activeControllers;
};

/*
 * Load all plugin shared libraries from a directory and register a
 * PluginDelegateManager for each one via AddControllerManager().
 *
 * Call this once at startup before constructing libera::System.
 */
void loadPluginsFromDirectory(const std::string& path);

} // namespace libera::plugin
