#include "PluginValidation.hpp"
#include "PluginSettingsInternal.hpp"

namespace libera::plugin {

bool isSharedLibraryPath(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".dylib" || ext == ".so" || ext == ".dll";
}

std::string validatePluginApi(const libera_plugin_api_t* api) {
    if (!api) {
        return "returned a null API table";
    }

    if (api->abi_version != LIBERA_PLUGIN_API_VERSION) {
        return "ABI version mismatch (plugin=" +
               std::to_string(api->abi_version) +
               ", host=" + std::to_string(LIBERA_PLUGIN_API_VERSION) + ")";
    }

    if (api->struct_size < LIBERA_PLUGIN_API_BASE_SIZE) {
        return "API table is smaller than the required transport interface";
    }

    if (!api->plugin_id || !*api->plugin_id) {
        return "missing plugin_id";
    }

    if (!api->plugin_version || !*api->plugin_version) {
        return "missing plugin_version";
    }

    if (!api->controller_type || !*api->controller_type) {
        return "missing controller_type";
    }

    if (!api->display_name || !*api->display_name) {
        return "missing display_name";
    }

    if (!api->discover) {
        return "missing discover()";
    }

    if (!api->connect_controller) {
        return "missing connect_controller()";
    }

    if (!api->destroy_controller) {
        return "missing destroy_controller()";
    }

    const bool hasPointTransport = api->send_points != nullptr;
    const bool hasFrameRequirements = api->get_frame_requirements != nullptr;
    const bool hasFrameSender = api->send_frame != nullptr;
    if (hasFrameRequirements != hasFrameSender) {
        return "must provide both get_frame_requirements() and send_frame()";
    }

    const bool hasFrameTransport = hasFrameRequirements && hasFrameSender;
    if (!hasPointTransport && !hasFrameTransport) {
        return "missing send_points() or get_frame_requirements()+send_frame()";
    }

    if (api->property_count > 0 && !api->properties) {
        return "declared properties without a property table";
    }

    if (api->property_count > 0 && !api->read_property) {
        return "declared properties without read_property()";
    }

    const bool hasSettingCount =
        LIBERA_PLUGIN_API_HAS_FIELD(api, get_setting_count) &&
        api->get_setting_count;
    const bool hasSettingDefinitions =
        LIBERA_PLUGIN_API_HAS_FIELD(api, get_setting_definition) &&
        api->get_setting_definition;
    if (hasSettingCount != hasSettingDefinitions) {
        return "settings must provide both definition callbacks";
    }

    if (hasSettingCount) {
        std::string settingsError;
        const auto pluginSettings = readSettingDefinitions(
            api, SettingScope::Plugin, &settingsError);
        if (!settingsError.empty()) {
            return "invalid plugin settings: " + settingsError;
        }
        const auto controllerSettings = readSettingDefinitions(
            api, SettingScope::Controller, &settingsError);
        if (!settingsError.empty()) {
            return "invalid controller settings: " + settingsError;
        }

        const bool hasPluginSetter =
            LIBERA_PLUGIN_API_HAS_FIELD(api, set_plugin_setting) &&
            api->set_plugin_setting;
        const bool hasControllerSetter =
            LIBERA_PLUGIN_API_HAS_FIELD(api, set_controller_setting) &&
            api->set_controller_setting;
        if (!pluginSettings.empty() && !hasPluginSetter) {
            return "declared plugin settings without set_plugin_setting()";
        }
        if (!controllerSettings.empty() && !hasControllerSetter) {
            return "declared controller settings without set_controller_setting()";
        }
    }

    return {};
}

} // namespace libera::plugin
