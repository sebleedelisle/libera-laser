#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libera::plugin {

enum class SettingType {
    Bool,
    Int,
    Float,
    String,
    Enum,
};

enum class SettingScope {
    Plugin,
    Controller,
};

struct SettingChoice {
    std::string value;
    std::string label;
};

struct SettingDefinition {
    SettingScope scope = SettingScope::Plugin;
    SettingType type = SettingType::String;
    std::string key;
    std::string label;
    std::string description;
    std::string defaultValue;
    std::optional<std::string> minimumValue;
    std::optional<std::string> maximumValue;
    std::optional<std::string> stepValue;
    std::vector<SettingChoice> choices;
};

struct Setting {
    SettingDefinition definition;
    std::string value;
};

struct SettingChangeResult {
    bool success = false;
    std::string message;
};

/*
 * Return settings declared by a loaded plugin. Controller definitions are
 * shared by every controller exposed by that plugin; controllerId selects the
 * persisted value for one stable discovered controller identity.
 */
std::vector<Setting> pluginSettings(const std::string& pluginId);
std::vector<Setting> controllerSettings(const std::string& pluginId,
                                        const std::string& controllerId);

/*
 * Apply a setting immediately when its target is live, then persist it. An
 * offline controller value is persisted and applied before its next streaming
 * thread starts. Every successful change also requests a plugin rescan.
 */
SettingChangeResult setPluginSetting(const std::string& pluginId,
                                     const std::string& key,
                                     const std::string& value);

SettingChangeResult setControllerSetting(const std::string& pluginId,
                                         const std::string& controllerId,
                                         const std::string& key,
                                         const std::string& value);

/*
 * Libera keeps plugin settings in the shared package store. This
 * accessor is mainly useful for diagnostics and settings-file backup tools.
 */
std::string pluginSettingsFilePath();

} // namespace libera::plugin
