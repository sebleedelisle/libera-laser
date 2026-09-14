#include "libera/plugin/libera_plugin.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

struct TestBackend {
    std::atomic<bool> alternateLabel{false};
    std::atomic<int> rescanCount{0};
    std::atomic<int> connectionCount{0};
};

struct TestController {
    TestBackend* backend = nullptr;
    std::atomic<int> gain{5};
    std::atomic<bool> invertX{false};
    std::atomic<double> scale{1.0};
    std::atomic<bool> disconnectNextSend{false};
    std::mutex nicknameMutex;
    std::string nickname;
};

void* createBackend(const libera_host_services_t*) {
    return new TestBackend;
}

void destroyBackend(void* rawBackend) {
    delete static_cast<TestBackend*>(rawBackend);
}

void rescan(void* rawBackend) {
    auto* backend = static_cast<TestBackend*>(rawBackend);
    if (backend) {
        backend->rescanCount.fetch_add(1);
    }
}

void discover(void* rawBackend, libera_emit_controller_fn emit, void* ctx) {
    if (!emit) {
        return;
    }

    auto* backend = static_cast<TestBackend*>(rawBackend);

    libera_controller_info_t info;
    libera_controller_info_init(&info,
                                "test-valid-001",
                                backend && backend->alternateLabel.load()
                                    ? "Test Alternate Controller"
                                    : "Test Valid Controller",
                                30000);
    emit(ctx, &info);
}

void* connectController(void* rawBackend,
                        const libera_controller_info_t*,
                        libera_host_ctx_t) {
    auto* controller = new TestController;
    controller->backend = static_cast<TestBackend*>(rawBackend);
    controller->backend->connectionCount.fetch_add(1);
    return controller;
}

void destroyController(void* rawController) {
    delete static_cast<TestController*>(rawController);
}

libera_status_t sendPoints(void* rawController,
                           const libera_point_t*,
                           uint32_t) {
    auto* controller = static_cast<TestController*>(rawController);
    if (controller && controller->disconnectNextSend.exchange(false)) {
        return LIBERA_ERR_DISCONNECTED;
    }
    return LIBERA_OK;
}

const libera_property_def_t properties[] = {
    {"rescans", "Rescans"},
    {"gain", "Gain"},
    {"connections", "Connections"},
    {"request_disconnect", "Request disconnect"},
};

int readProperty(void* rawController,
                 uint32_t propertyIndex,
                 char* out,
                 uint32_t outSize) {
    auto* controller = static_cast<TestController*>(rawController);
    if (!controller || !controller->backend) {
        return -1;
    }

    int value = 0;
    switch (propertyIndex) {
        case 0:
            value = controller->backend->rescanCount.load();
            break;
        case 1:
            value = controller->gain.load();
            break;
        case 2:
            value = controller->backend->connectionCount.load();
            break;
        case 3:
            // This test-only diagnostic property makes the next submission
            // fail so the host's automatic reconnect path can be exercised.
            controller->disconnectNextSend.store(true);
            libera_copy_string(out, outSize, "requested");
            return 9;
        default:
            return -1;
    }

    char buffer[32];
    const int length = std::snprintf(buffer, sizeof(buffer), "%d", value);
    if (out && outSize > 0) {
        libera_copy_string(out, outSize, buffer);
    }
    return length;
}

const libera_setting_choice_t labelChoices[] = {
    {"standard", "Standard"},
    {"alternate", "Alternate"},
};

const libera_setting_def_t pluginSetting = {
    /* struct_size   */ sizeof(libera_setting_def_t),
    /* key           */ "discovery_label",
    /* label         */ "Discovery label",
    /* description   */ "Selects the label exposed by the test fixture.",
    /* type          */ LIBERA_SETTING_ENUM,
    /* default_value */ "standard",
    /* minimum_value */ nullptr,
    /* maximum_value */ nullptr,
    /* step_value    */ nullptr,
    /* choices       */ labelChoices,
    /* choice_count  */ 2,
};

const libera_setting_def_t controllerSetting = {
    /* struct_size   */ sizeof(libera_setting_def_t),
    /* key           */ "gain",
    /* label         */ "Gain",
    /* description   */ "A test-only controller gain.",
    /* type          */ LIBERA_SETTING_INT,
    /* default_value */ "5",
    /* minimum_value */ "0",
    /* maximum_value */ "10",
    /* step_value    */ "1",
    /* choices       */ nullptr,
    /* choice_count  */ 0,
};

const libera_setting_def_t invertXSetting = {
    /* struct_size   */ sizeof(libera_setting_def_t),
    /* key           */ "invert_x",
    /* label         */ "Invert X",
    /* description   */ "A test-only boolean setting.",
    /* type          */ LIBERA_SETTING_BOOL,
    /* default_value */ "false",
    /* minimum_value */ nullptr,
    /* maximum_value */ nullptr,
    /* step_value    */ nullptr,
    /* choices       */ nullptr,
    /* choice_count  */ 0,
};

const libera_setting_def_t scaleSetting = {
    /* struct_size   */ sizeof(libera_setting_def_t),
    /* key           */ "scale",
    /* label         */ "Scale",
    /* description   */ "A test-only floating-point setting.",
    /* type          */ LIBERA_SETTING_FLOAT,
    /* default_value */ "1.0",
    /* minimum_value */ "0.0",
    /* maximum_value */ "2.0",
    /* step_value    */ "0.1",
    /* choices       */ nullptr,
    /* choice_count  */ 0,
};

const libera_setting_def_t nicknameSetting = {
    /* struct_size   */ sizeof(libera_setting_def_t),
    /* key           */ "nickname",
    /* label         */ "Nickname",
    /* description   */ "A test-only string setting.",
    /* type          */ LIBERA_SETTING_STRING,
    /* default_value */ "",
    /* minimum_value */ nullptr,
    /* maximum_value */ nullptr,
    /* step_value    */ nullptr,
    /* choices       */ nullptr,
    /* choice_count  */ 0,
};

uint32_t getSettingCount(libera_setting_scope_t scope) {
    return scope == LIBERA_SETTING_SCOPE_PLUGIN ? 1 : 4;
}

const libera_setting_def_t* getSettingDefinition(
    libera_setting_scope_t scope,
    uint32_t settingIndex) {
    if (scope == LIBERA_SETTING_SCOPE_PLUGIN) {
        return settingIndex == 0 ? &pluginSetting : nullptr;
    }
    switch (settingIndex) {
        case 0:
            return &controllerSetting;
        case 1:
            return &invertXSetting;
        case 2:
            return &scaleSetting;
        case 3:
            return &nicknameSetting;
        default:
            return nullptr;
    }
}

libera_status_t setPluginSetting(void* rawBackend,
                                 const char* key,
                                 const char* value) {
    auto* backend = static_cast<TestBackend*>(rawBackend);
    if (!backend || !key || !value ||
        std::strcmp(key, "discovery_label") != 0) {
        return LIBERA_ERR_INVALID_ARGUMENT;
    }
    if (std::strcmp(value, "standard") == 0) {
        backend->alternateLabel.store(false);
        return LIBERA_OK;
    }
    if (std::strcmp(value, "alternate") == 0) {
        backend->alternateLabel.store(true);
        return LIBERA_OK;
    }
    return LIBERA_ERR_INVALID_ARGUMENT;
}

libera_status_t setControllerSetting(void* rawController,
                                     const char* key,
                                     const char* value) {
    auto* controller = static_cast<TestController*>(rawController);
    if (!controller || !key || !value) {
        return LIBERA_ERR_INVALID_ARGUMENT;
    }

    if (std::strcmp(key, "gain") == 0) {
        int gain = -1;
        if (std::sscanf(value, "%d", &gain) != 1 || gain < 0 || gain > 10) {
            return LIBERA_ERR_INVALID_ARGUMENT;
        }
        controller->gain.store(gain);
        return LIBERA_OK;
    }
    if (std::strcmp(key, "invert_x") == 0) {
        if (std::strcmp(value, "true") == 0) {
            controller->invertX.store(true);
            return LIBERA_OK;
        }
        if (std::strcmp(value, "false") == 0) {
            controller->invertX.store(false);
            return LIBERA_OK;
        }
        return LIBERA_ERR_INVALID_ARGUMENT;
    }
    if (std::strcmp(key, "scale") == 0) {
        double scale = -1.0;
        if (std::sscanf(value, "%lf", &scale) != 1 || scale < 0.0 || scale > 2.0) {
            return LIBERA_ERR_INVALID_ARGUMENT;
        }
        controller->scale.store(scale);
        return LIBERA_OK;
    }
    if (std::strcmp(key, "nickname") == 0) {
        std::lock_guard lock(controller->nicknameMutex);
        controller->nickname = value;
        return LIBERA_OK;
    }
    return LIBERA_ERR_INVALID_ARGUMENT;
}

const libera_plugin_api_t pluginApi = {
    /* abi_version        */ LIBERA_PLUGIN_API_VERSION,
    /* struct_size        */ sizeof(libera_plugin_api_t),
    /* type_name          */ "TestValidPlugin",
    /* display_name       */ "Test Valid Plugin",
    /* create_backend     */ &createBackend,
    /* destroy_backend    */ &destroyBackend,
    /* rescan             */ &rescan,
    /* discover           */ &discover,
    /* connect_controller */ &connectController,
    /* destroy_controller */ &destroyController,
    /* send_points        */ &sendPoints,
    /* set_point_rate     */ nullptr,
    /* set_armed          */ nullptr,
    /* get_buffer_state   */ nullptr,
    /* properties         */ properties,
    /* property_count     */ 4,
    /* read_property      */ &readProperty,
    /* get_frame_requirements */ nullptr,
    /* send_frame             */ nullptr,
    /* get_setting_count      */ &getSettingCount,
    /* get_setting_definition */ &getSettingDefinition,
    /* set_plugin_setting     */ &setPluginSetting,
    /* set_controller_setting */ &setControllerSetting,
};

} // namespace

LIBERA_PLUGIN_EXPORT(pluginApi)
