#include "libera/plugin/libera_plugin.h"

namespace {

void discover(void*, libera_emit_controller_fn, void*) {}

void* connectController(void*,
                        const libera_controller_info_t*,
                        libera_host_ctx_t) {
    return nullptr;
}

void destroyController(void*) {}

const libera_plugin_api_t pluginApi = {
    /* abi_version        */ LIBERA_PLUGIN_API_VERSION,
    /* struct_size        */ sizeof(libera_plugin_api_t),
    /* plugin_id          */ "org.libera.test-missing-transport",
    /* plugin_version     */ "1.0.0",
    /* controller_type    */ "TestMissingTransportPlugin",
    /* display_name       */ "Test Missing Transport Plugin",
    /* create_backend     */ nullptr,
    /* destroy_backend    */ nullptr,
    /* rescan             */ nullptr,
    /* discover           */ &discover,
    /* connect_controller */ &connectController,
    /* destroy_controller */ &destroyController,
    /* send_points        */ nullptr,
    /* set_point_rate     */ nullptr,
    /* set_armed          */ nullptr,
    /* get_buffer_state   */ nullptr,
    /* properties         */ nullptr,
    /* property_count     */ 0,
    /* read_property      */ nullptr,
    /* get_frame_requirements */ nullptr,
    /* send_frame             */ nullptr,
    /* get_setting_count      */ nullptr,
    /* get_setting_definition */ nullptr,
    /* set_plugin_setting     */ nullptr,
    /* set_controller_setting */ nullptr,
};

} // namespace

LIBERA_PLUGIN_EXPORT(pluginApi)
