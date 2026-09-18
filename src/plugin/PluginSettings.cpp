#include "libera/plugin/PluginSettings.hpp"

#include "libera/System.hpp"
#include "libera/log/Log.hpp"
#include "libera/plugin/PluginController.hpp"
#include "libera/plugin/PluginManager.hpp"
#include "libera/plugin/PluginRegistry.hpp"

#include "PluginSettingsInternal.hpp"
#include "PluginFileLock.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace libera::plugin {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

struct SettingAddress {
    std::string pluginId;
    std::string controllerId;
    std::string key;

    bool operator==(const SettingAddress& other) const {
        return pluginId == other.pluginId &&
               controllerId == other.controllerId &&
               key == other.key;
    }
};

struct SettingAddressHash {
    std::size_t operator()(const SettingAddress& address) const {
        const auto combine = [](std::size_t seed, const std::string& value) {
            const std::size_t hashed = std::hash<std::string>{}(value);
            return seed ^ (hashed + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
        };

        std::size_t result = combine(0, address.pluginId);
        result = combine(result, address.controllerId);
        return combine(result, address.key);
    }
};

class SettingsStore {
public:
    std::optional<std::string> get(const SettingAddress& address) {
        std::lock_guard lock(mutex);
        ensureLoaded();
        const auto it = values.find(address);
        return it == values.end()
            ? std::nullopt
            : std::optional<std::string>(it->second);
    }

    bool set(const SettingAddress& address,
             const std::string& value,
             std::string* error) {
        std::lock_guard lock(mutex);
        const std::string currentPath = pluginSettingsFilePath();
        if (currentPath.empty()) {
            if (error) {
                *error = "Plugin settings path is unavailable";
            }
            return false;
        }

        std::error_code ec;
        const fs::path destination = fs::u8path(currentPath);
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            if (error) {
                *error = "Failed to create settings directory: " + ec.message();
            }
            return false;
        }
        ExclusivePluginFileLock fileLock(
            destination.parent_path() / ".store.lock");
        if (!fileLock.locked()) {
            if (error) *error = fileLock.error();
            return false;
        }

        ensureLoaded(true);
        if (!loadedFileValid) {
            if (error) {
                *error = "Plugin settings.json is invalid; refusing to overwrite it";
            }
            return false;
        }

        const auto previous = values.find(address);
        const bool hadPrevious = previous != values.end();
        const std::string previousValue = hadPrevious
            ? previous->second
            : std::string{};
        values[address] = value;

        if (writeFile(error)) {
            return true;
        }

        // Keep the in-memory state consistent with the file when persistence
        // fails so callers can safely roll a live plugin back to its old value.
        if (hadPrevious) {
            values[address] = previousValue;
        } else {
            values.erase(address);
        }
        return false;
    }

private:
    void ensureLoaded(bool force = false) {
        const std::string currentPath = pluginSettingsFilePath();
        if (!force && currentPath == loadedPath) {
            return;
        }

        loadedPath = currentPath;
        values.clear();
        loadedFileValid = true;
        if (loadedPath.empty()) {
            return;
        }

        std::ifstream input{fs::u8path(loadedPath)};
        if (!input) {
            return;
        }

        try {
            Json document;
            input >> document;
            input >> std::ws;
            if (!input.eof() || !document.is_object() ||
                document.value("schemaVersion", 0) != 1 ||
                !document.contains("values") ||
                !document["values"].is_array()) {
                loadedFileValid = false;
                return;
            }
            for (const auto& item : document["values"]) {
                if (!item.is_object() ||
                    !item.contains("pluginId") || !item["pluginId"].is_string() ||
                    !item.contains("controllerId") ||
                    !item["controllerId"].is_string() ||
                    !item.contains("key") || !item["key"].is_string() ||
                    !item.contains("value") || !item["value"].is_string()) {
                    loadedFileValid = false;
                    values.clear();
                    return;
                }
                const auto pluginId = item.value("pluginId", std::string{});
                const auto controllerId = item.value("controllerId", std::string{});
                const auto key = item.value("key", std::string{});
                const auto value = item.value("value", std::string{});
                if (pluginId.empty() || key.empty()) {
                    loadedFileValid = false;
                    values.clear();
                    return;
                }
                values[{pluginId, controllerId, key}] = value;
            }
        } catch (const std::exception&) {
            values.clear();
            loadedFileValid = false;
        }
    }

    bool writeFile(std::string* error) {
        std::error_code ec;
        const fs::path destination = fs::u8path(loadedPath);
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            if (error) {
                *error = "Failed to create settings directory: " + ec.message();
            }
            return false;
        }

        std::vector<std::pair<SettingAddress, std::string>> sortedValues(
            values.begin(), values.end());
        std::sort(sortedValues.begin(), sortedValues.end(),
                  [](const auto& a, const auto& b) {
                      return std::tie(a.first.pluginId,
                                      a.first.controllerId,
                                      a.first.key) <
                             std::tie(b.first.pluginId,
                                      b.first.controllerId,
                                      b.first.key);
                  });

        const auto suffix = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        fs::path temporary = destination;
        temporary += ".tmp." + std::to_string(suffix);

        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) {
                if (error) {
                    *error = "Failed to open temporary settings file";
                }
                return false;
            }

            Json document = {
                {"schemaVersion", 1},
                {"values", Json::array()},
            };
            for (const auto& [address, value] : sortedValues) {
                document["values"].push_back({
                    {"pluginId", address.pluginId},
                    {"controllerId", address.controllerId},
                    {"key", address.key},
                    {"value", value},
                });
            }
            output << document.dump(2) << '\n';
            output.flush();
            if (!output) {
                if (error) {
                    *error = "Failed to write plugin settings";
                }
                output.close();
                fs::remove(temporary, ec);
                return false;
            }
        }

#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(),
                         destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (error) {
                *error = "Failed to replace plugin settings file";
            }
            fs::remove(temporary, ec);
            return false;
        }
#else
        fs::rename(temporary, destination, ec);
        if (ec) {
            if (error) {
                *error = "Failed to replace plugin settings file: " + ec.message();
            }
            fs::remove(temporary, ec);
            return false;
        }
#endif
        return true;
    }

    std::mutex mutex;
    std::string loadedPath;
    bool loadedFileValid = true;
    std::unordered_map<SettingAddress, std::string, SettingAddressHash> values;
};

struct RuntimeState {
    std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<LoadedPlugin>> plugins;
    std::unordered_map<SettingAddress,
                       std::weak_ptr<PluginController>,
                       SettingAddressHash> controllers;
};

RuntimeState& runtimeState() {
    static RuntimeState state;
    return state;
}

SettingsStore& settingsStore() {
    static SettingsStore store;
    return store;
}

const char* safeString(const char* value) {
    return value ? value : "";
}

libera_setting_scope_t toAbiScope(SettingScope scope) {
    return scope == SettingScope::Plugin
        ? LIBERA_SETTING_SCOPE_PLUGIN
        : LIBERA_SETTING_SCOPE_CONTROLLER;
}

bool parseInteger(const std::string& text, std::int64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parseFloat(const std::string& text, double& value) {
    if (text.empty()) {
        return false;
    }

    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> value;
    return input && input.peek() == std::char_traits<char>::eof() &&
           std::isfinite(value);
}

bool validateNumericMetadata(const SettingDefinition& definition,
                             std::string* error) {
    if (definition.type != SettingType::Int &&
        definition.type != SettingType::Float) {
        if (definition.minimumValue || definition.maximumValue ||
            definition.stepValue) {
            if (error) {
                *error = "non-numeric setting declares numeric constraints";
            }
            return false;
        }
        return true;
    }

    if (definition.type == SettingType::Int) {
        std::int64_t minimum = 0;
        std::int64_t maximum = 0;
        std::int64_t step = 0;
        if ((definition.minimumValue &&
             !parseInteger(*definition.minimumValue, minimum)) ||
            (definition.maximumValue &&
             !parseInteger(*definition.maximumValue, maximum)) ||
            (definition.stepValue &&
             (!parseInteger(*definition.stepValue, step) || step <= 0))) {
            if (error) {
                *error = "integer setting has invalid bounds or step";
            }
            return false;
        }
        if (definition.minimumValue && definition.maximumValue &&
            minimum > maximum) {
            if (error) {
                *error = "integer setting minimum exceeds maximum";
            }
            return false;
        }
        return true;
    }

    double minimum = 0.0;
    double maximum = 0.0;
    double step = 0.0;
    if ((definition.minimumValue && !parseFloat(*definition.minimumValue, minimum)) ||
        (definition.maximumValue && !parseFloat(*definition.maximumValue, maximum)) ||
        (definition.stepValue &&
         (!parseFloat(*definition.stepValue, step) || step <= 0.0))) {
        if (error) {
            *error = "floating-point setting has invalid bounds or step";
        }
        return false;
    }
    if (definition.minimumValue && definition.maximumValue && minimum > maximum) {
        if (error) {
            *error = "floating-point setting minimum exceeds maximum";
        }
        return false;
    }
    return true;
}

bool settingsApiAvailable(const libera_plugin_api_t* api) {
    return LIBERA_PLUGIN_API_HAS_FIELD(api, get_setting_count) &&
           LIBERA_PLUGIN_API_HAS_FIELD(api, get_setting_definition);
}

std::shared_ptr<LoadedPlugin> findPlugin(const std::string& pluginId) {
    auto& state = runtimeState();
    std::lock_guard lock(state.mutex);
    const auto it = state.plugins.find(pluginId);
    if (it == state.plugins.end()) {
        return nullptr;
    }
    return it->second.lock();
}

std::shared_ptr<PluginController> findController(
    const std::string& pluginId,
    const std::string& controllerId) {
    auto& state = runtimeState();
    std::lock_guard lock(state.mutex);
    const SettingAddress address{pluginId, controllerId, {}};
    const auto it = state.controllers.find(address);
    if (it == state.controllers.end()) {
        return nullptr;
    }
    return it->second.lock();
}

const SettingDefinition* findDefinition(
    const std::vector<SettingDefinition>& definitions,
    const std::string& key) {
    const auto it = std::find_if(definitions.begin(), definitions.end(),
                                 [&](const SettingDefinition& definition) {
                                     return definition.key == key;
                                 });
    return it == definitions.end() ? nullptr : &*it;
}

std::string resolvedValue(const SettingDefinition& definition,
                          const SettingAddress& address) {
    const auto saved = settingsStore().get(address);
    if (saved && validateSettingValue(definition, *saved)) {
        return *saved;
    }
    return definition.defaultValue;
}

const char* describeStatus(libera_status_t status) {
    switch (status) {
        case LIBERA_OK:
            return "ok";
        case LIBERA_ERR_DISCONNECTED:
            return "disconnected";
        case LIBERA_ERR_TIMEOUT:
            return "timeout";
        case LIBERA_ERR_BUSY:
            return "busy";
        case LIBERA_ERR_PROTOCOL:
            return "protocol";
        case LIBERA_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case LIBERA_ERR_INTERNAL:
            return "internal error";
    }
    return "unknown error";
}

std::vector<Setting> settingsFor(const std::shared_ptr<LoadedPlugin>& plugin,
                                 SettingScope scope,
                                 const std::string& controllerId) {
    if (!plugin || !plugin->api) {
        return {};
    }

    std::string schemaError;
    auto definitions = readSettingDefinitions(plugin->api, scope, &schemaError);
    if (!schemaError.empty()) {
        PluginRegistry::instance().pushRuntimeError(
            plugin->entrypointPath, "settings.schema", schemaError);
        return {};
    }

    std::vector<Setting> result;
    result.reserve(definitions.size());
    for (auto& definition : definitions) {
        SettingAddress address{
            plugin->pluginId,
            scope == SettingScope::Controller ? controllerId : std::string{},
            definition.key,
        };
        result.push_back({definition, resolvedValue(definition, address)});
    }
    return result;
}

void requestRescan(const std::shared_ptr<LoadedPlugin>& plugin) {
    if (plugin && plugin->initialised && plugin->api && plugin->api->rescan) {
        plugin->api->rescan(plugin->backendHandle);
    }
}

} // namespace

std::vector<SettingDefinition> readSettingDefinitions(
    const libera_plugin_api_t* api,
    SettingScope scope,
    std::string* error) {
    if (error) {
        error->clear();
    }
    if (!settingsApiAvailable(api)) {
        return {};
    }
    if (!api->get_setting_count && !api->get_setting_definition) {
        return {};
    }
    if (!api->get_setting_count || !api->get_setting_definition) {
        if (error) {
            *error = "settings must provide both definition callbacks";
        }
        return {};
    }

    const auto abiScope = toAbiScope(scope);
    const std::uint32_t count = api->get_setting_count(abiScope);
    std::vector<SettingDefinition> definitions;
    definitions.reserve(count);
    std::unordered_set<std::string> keys;

    for (std::uint32_t i = 0; i < count; ++i) {
        const libera_setting_def_t* raw =
            api->get_setting_definition(abiScope, i);
        if (!raw || raw->struct_size < LIBERA_PLUGIN_SETTING_DEF_BASE_SIZE) {
            if (error) {
                *error = "setting definition " + std::to_string(i) +
                         " is missing or too small";
            }
            return {};
        }
        if (!raw->default_value) {
            if (error) {
                *error = "setting definition " + std::to_string(i) +
                         " has no default value";
            }
            return {};
        }

        SettingDefinition definition;
        definition.scope = scope;
        definition.key = safeString(raw->key);
        definition.label = safeString(raw->label);
        definition.description = safeString(raw->description);
        definition.defaultValue = safeString(raw->default_value);

        switch (raw->type) {
            case LIBERA_SETTING_BOOL:
                definition.type = SettingType::Bool;
                break;
            case LIBERA_SETTING_INT:
                definition.type = SettingType::Int;
                break;
            case LIBERA_SETTING_FLOAT:
                definition.type = SettingType::Float;
                break;
            case LIBERA_SETTING_STRING:
                definition.type = SettingType::String;
                break;
            case LIBERA_SETTING_ENUM:
                definition.type = SettingType::Enum;
                break;
            default:
                if (error) {
                    *error = "setting " + std::to_string(i) +
                             " has an unknown type";
                }
                return {};
        }

        if (raw->minimum_value) {
            definition.minimumValue = raw->minimum_value;
        }
        if (raw->maximum_value) {
            definition.maximumValue = raw->maximum_value;
        }
        if (raw->step_value) {
            definition.stepValue = raw->step_value;
        }

        if (raw->choice_count > 0 && !raw->choices) {
            if (error) {
                *error = "setting " + definition.key +
                         " declares choices without a choice table";
            }
            return {};
        }
        std::unordered_set<std::string> choiceValues;
        for (std::uint32_t choiceIndex = 0;
             choiceIndex < raw->choice_count;
             ++choiceIndex) {
            SettingChoice choice{
                safeString(raw->choices[choiceIndex].value),
                safeString(raw->choices[choiceIndex].label),
            };
            if (choice.value.empty() || choice.label.empty() ||
                !choiceValues.insert(choice.value).second) {
                if (error) {
                    *error = "setting " + definition.key +
                             " has an invalid or duplicate choice";
                }
                return {};
            }
            definition.choices.push_back(std::move(choice));
        }

        if (definition.key.empty() || definition.label.empty() ||
            !keys.insert(definition.key).second) {
            if (error) {
                *error = "setting definitions need unique non-empty keys and labels";
            }
            return {};
        }
        if (definition.type == SettingType::Enum && definition.choices.empty()) {
            if (error) {
                *error = "enum setting " + definition.key + " has no choices";
            }
            return {};
        }
        if (definition.type != SettingType::Enum && !definition.choices.empty()) {
            if (error) {
                *error = "non-enum setting " + definition.key +
                         " declares choices";
            }
            return {};
        }

        std::string metadataError;
        if (!validateNumericMetadata(definition, &metadataError) ||
            !validateSettingValue(definition,
                                  definition.defaultValue,
                                  &metadataError)) {
            if (error) {
                *error = "setting " + definition.key + ": " + metadataError;
            }
            return {};
        }

        definitions.push_back(std::move(definition));
    }

    return definitions;
}

bool validateSettingValue(const SettingDefinition& definition,
                          const std::string& value,
                          std::string* error) {
    if (error) {
        error->clear();
    }

    switch (definition.type) {
        case SettingType::Bool:
            if (value == "true" || value == "false") {
                return true;
            }
            if (error) {
                *error = "expected true or false";
            }
            return false;

        case SettingType::Int: {
            std::int64_t parsed = 0;
            if (!parseInteger(value, parsed)) {
                if (error) {
                    *error = "expected a base-10 integer";
                }
                return false;
            }
            std::int64_t bound = 0;
            if (definition.minimumValue &&
                parseInteger(*definition.minimumValue, bound) && parsed < bound) {
                if (error) {
                    *error = "value is below the minimum";
                }
                return false;
            }
            if (definition.maximumValue &&
                parseInteger(*definition.maximumValue, bound) && parsed > bound) {
                if (error) {
                    *error = "value is above the maximum";
                }
                return false;
            }
            return true;
        }

        case SettingType::Float: {
            double parsed = 0.0;
            if (!parseFloat(value, parsed)) {
                if (error) {
                    *error = "expected a finite decimal number";
                }
                return false;
            }
            double bound = 0.0;
            if (definition.minimumValue &&
                parseFloat(*definition.minimumValue, bound) && parsed < bound) {
                if (error) {
                    *error = "value is below the minimum";
                }
                return false;
            }
            if (definition.maximumValue &&
                parseFloat(*definition.maximumValue, bound) && parsed > bound) {
                if (error) {
                    *error = "value is above the maximum";
                }
                return false;
            }
            return true;
        }

        case SettingType::String:
            return true;

        case SettingType::Enum:
            if (std::any_of(definition.choices.begin(),
                            definition.choices.end(),
                            [&](const SettingChoice& choice) {
                                return choice.value == value;
                            })) {
                return true;
            }
            if (error) {
                *error = "value is not one of the declared choices";
            }
            return false;
    }

    if (error) {
        *error = "unknown setting type";
    }
    return false;
}

void registerLoadedPlugin(const std::shared_ptr<LoadedPlugin>& plugin) {
    if (!plugin || plugin->pluginId.empty()) {
        return;
    }
    auto& state = runtimeState();
    std::lock_guard lock(state.mutex);
    const auto existing = state.plugins.find(plugin->pluginId);
    if (existing == state.plugins.end() || existing->second.expired()) {
        state.plugins[plugin->pluginId] = plugin;
    }
}

void registerPluginController(
    const std::string& pluginId,
    const std::string& controllerId,
    const std::shared_ptr<PluginController>& controller) {
    if (pluginId.empty() || controllerId.empty() || !controller) {
        return;
    }
    auto& state = runtimeState();
    std::lock_guard lock(state.mutex);
    state.controllers[{pluginId, controllerId, {}}] = controller;
}

bool applySavedPluginSettings(const std::shared_ptr<LoadedPlugin>& plugin,
                              std::string* error) {
    if (error) {
        error->clear();
    }
    if (!plugin || !plugin->api) {
        if (error) {
            *error = "Plugin is unavailable";
        }
        return false;
    }

    std::string schemaError;
    const auto definitions = readSettingDefinitions(
        plugin->api, SettingScope::Plugin, &schemaError);
    if (!schemaError.empty()) {
        if (error) {
            *error = schemaError;
        }
        return false;
    }
    if (definitions.empty()) {
        return true;
    }

    if (!LIBERA_PLUGIN_API_HAS_FIELD(plugin->api, set_plugin_setting) ||
        !plugin->api->set_plugin_setting) {
        if (error) {
            *error = "Plugin settings have no setter";
        }
        return false;
    }

    for (const auto& definition : definitions) {
        const SettingAddress address{plugin->pluginId, {}, definition.key};
        const std::string value = resolvedValue(definition, address);
        const auto status = plugin->api->set_plugin_setting(
            plugin->backendHandle, definition.key.c_str(), value.c_str());
        if (status != LIBERA_OK) {
            if (error) {
                *error = "Setting " + definition.key + " was rejected: " +
                         describeStatus(status);
            }
            return false;
        }
    }
    return true;
}

bool applySavedControllerSettings(PluginController& controller,
                                  std::string* error) {
    if (error) {
        error->clear();
    }
    const auto plugin = findPlugin(controller.pluginId());
    if (!plugin || !plugin->api) {
        if (error) {
            *error = "Plugin is unavailable";
        }
        return false;
    }

    std::string schemaError;
    const auto definitions = readSettingDefinitions(
        plugin->api, SettingScope::Controller, &schemaError);
    if (!schemaError.empty()) {
        if (error) {
            *error = schemaError;
        }
        return false;
    }

    for (const auto& definition : definitions) {
        const SettingAddress address{
            controller.pluginId(), controller.controllerId(), definition.key};
        const std::string value = resolvedValue(definition, address);
        const auto status = controller.applySetting(definition.key, value);
        if (status != LIBERA_OK) {
            if (error) {
                *error = "Setting " + definition.key + " was rejected: " +
                         describeStatus(status);
            }
            return false;
        }
    }
    return true;
}

std::vector<Setting> pluginSettings(const std::string& pluginId) {
    return settingsFor(findPlugin(pluginId), SettingScope::Plugin, {});
}

std::vector<Setting> controllerSettings(const std::string& pluginId,
                                        const std::string& controllerId) {
    if (controllerId.empty()) {
        return {};
    }
    return settingsFor(findPlugin(pluginId),
                       SettingScope::Controller,
                       controllerId);
}

SettingChangeResult setPluginSetting(const std::string& pluginId,
                                     const std::string& key,
                                     const std::string& value) {
    const auto plugin = findPlugin(pluginId);
    if (!plugin || !plugin->api) {
        return {false, "Plugin is not loaded"};
    }

    std::string schemaError;
    const auto definitions = readSettingDefinitions(
        plugin->api, SettingScope::Plugin, &schemaError);
    if (!schemaError.empty()) {
        return {false, schemaError};
    }
    const auto* definition = findDefinition(definitions, key);
    if (!definition) {
        return {false, "Unknown plugin setting: " + key};
    }

    std::string validationError;
    if (!validateSettingValue(*definition, value, &validationError)) {
        return {false, "Invalid value for " + key + ": " + validationError};
    }

    const SettingAddress address{pluginId, {}, key};
    const std::string oldValue = resolvedValue(*definition, address);
    std::lock_guard lifecycleLock(plugin->lifecycleMutex);

    if (plugin->initialised) {
        if (!LIBERA_PLUGIN_API_HAS_FIELD(plugin->api, set_plugin_setting) ||
            !plugin->api->set_plugin_setting) {
            return {false, "Plugin setting has no setter"};
        }
        const auto status = plugin->api->set_plugin_setting(
            plugin->backendHandle, key.c_str(), value.c_str());
        if (status != LIBERA_OK) {
            return {false, "Plugin rejected the setting: " +
                           std::string(describeStatus(status))};
        }
    }

    std::string persistenceError;
    if (!settingsStore().set(address, value, &persistenceError)) {
        if (plugin->initialised && plugin->api->set_plugin_setting) {
            plugin->api->set_plugin_setting(plugin->backendHandle,
                                            key.c_str(),
                                            oldValue.c_str());
        }
        return {false, persistenceError};
    }

    requestRescan(plugin);
    return {true, "Setting updated"};
}

SettingChangeResult setControllerSetting(const std::string& pluginId,
                                         const std::string& controllerId,
                                         const std::string& key,
                                         const std::string& value) {
    const auto plugin = findPlugin(pluginId);
    if (!plugin || !plugin->api) {
        return {false, "Plugin is not loaded"};
    }
    if (controllerId.empty()) {
        return {false, "Controller ID is empty"};
    }

    std::string schemaError;
    const auto definitions = readSettingDefinitions(
        plugin->api, SettingScope::Controller, &schemaError);
    if (!schemaError.empty()) {
        return {false, schemaError};
    }
    const auto* definition = findDefinition(definitions, key);
    if (!definition) {
        return {false, "Unknown controller setting: " + key};
    }

    std::string validationError;
    if (!validateSettingValue(*definition, value, &validationError)) {
        return {false, "Invalid value for " + key + ": " + validationError};
    }

    const SettingAddress address{pluginId, controllerId, key};
    const std::string oldValue = resolvedValue(*definition, address);
    const auto controller = findController(pluginId, controllerId);
    std::lock_guard lifecycleLock(plugin->lifecycleMutex);

    bool appliedLive = false;
    if (controller) {
        const auto status = controller->applySetting(key, value);
        if (status == LIBERA_OK) {
            appliedLive = true;
        } else if (status != LIBERA_ERR_DISCONNECTED) {
            return {false, "Controller rejected the setting: " +
                           std::string(describeStatus(status))};
        }
    }

    std::string persistenceError;
    if (!settingsStore().set(address, value, &persistenceError)) {
        if (appliedLive) {
            controller->applySetting(key, oldValue);
        }
        return {false, persistenceError};
    }

    requestRescan(plugin);
    return {true, appliedLive
        ? "Setting updated"
        : "Setting saved for the next connection"};
}

std::string pluginSettingsFilePath() {
    const std::string& directory = System::pluginDirectory();
    if (directory.empty()) {
        return {};
    }
    return (fs::u8path(directory) / "settings.json").u8string();
}

} // namespace libera::plugin
