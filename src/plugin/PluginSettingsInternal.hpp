#pragma once

#include "libera/plugin/PluginSettings.hpp"
#include "libera/plugin/libera_plugin.h"

#include <memory>
#include <string>
#include <vector>

namespace libera::plugin {

struct LoadedPlugin;
class PluginController;

// These helpers keep the native handles private to the plugin adapter while
// allowing the public settings API and validation code to share one schema
// implementation.
std::vector<SettingDefinition> readSettingDefinitions(
    const libera_plugin_api_t* api,
    SettingScope scope,
    std::string* error = nullptr);

bool validateSettingValue(const SettingDefinition& definition,
                          const std::string& value,
                          std::string* error = nullptr);

void registerLoadedPlugin(const std::shared_ptr<LoadedPlugin>& plugin);
void registerPluginController(const std::string& pluginType,
                              const std::string& controllerId,
                              const std::shared_ptr<PluginController>& controller);

bool applySavedPluginSettings(const std::shared_ptr<LoadedPlugin>& plugin,
                              std::string* error = nullptr);
bool applySavedControllerSettings(PluginController& controller,
                                  std::string* error = nullptr);

} // namespace libera::plugin
