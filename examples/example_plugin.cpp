/*
 * example_plugin — minimal libera DAC plugin.
 *
 * This plugin simulates a single DAC that accepts and discards points.
 * Use it as a starting point for writing your own plugin, or to verify
 * that plugin loading and the discover/connect/send/disconnect cycle
 * works end-to-end without hardware.
 *
 * Build as a shared library and place it in the plugins directory:
 *
 *   # macOS / Linux
 *   c++ -shared -fPIC -std=c++17 -o example-plugin.dylib example_plugin.cpp \
 *       -I ../include
 *
 *   # Windows (MSVC)
 *   cl /LD /std:c++17 /I ..\include example_plugin.cpp /Fe:example-plugin.dll
 */

#include "libera/plugin/libera_plugin.h"

#include <atomic>
#include <cstring>

namespace {

struct ExampleHandle {
    std::atomic<bool> armed{false};
};

} // anonymous namespace

extern "C" {

uint32_t libera_plugin_api_version(void) {
    return LIBERA_PLUGIN_API_VERSION;
}

const char* libera_plugin_type(void) {
    return "Example";
}

const char* libera_plugin_name(void) {
    return "Example Plugin";
}

int libera_plugin_init(void) {
    // No global state to set up.
    return 0;
}

void libera_plugin_shutdown(void) {
    // Nothing to tear down.
}

int libera_plugin_discover(libera_controller_info_t* out, int max_count) {
    if (max_count < 1) return 0;

    // Report one simulated controller.
    std::strncpy(out[0].id, "example-dac-1", sizeof(out[0].id));
    std::strncpy(out[0].label, "Example DAC", sizeof(out[0].label));
    out[0].maxPointRate = 30000;
    return 1;
}

void* libera_plugin_connect(const char* controller_id) {
    if (std::strcmp(controller_id, "example-dac-1") != 0) {
        return nullptr;
    }
    return new ExampleHandle;
}

int libera_plugin_send_points(void*                 handle,
                              const libera_point_t* /*points*/,
                              uint32_t              /*count*/,
                              uint32_t              /*point_rate*/) {
    auto* h = static_cast<ExampleHandle*>(handle);
    if (!h) return -1;
    // A real plugin would convert and send points here.
    return 0;
}

int libera_plugin_get_buffer_fullness(void* /*handle*/) {
    // Return -1 when buffer state is not available.
    return -1;
}

void libera_plugin_set_armed(void* handle, bool armed) {
    auto* h = static_cast<ExampleHandle*>(handle);
    if (h) h->armed.store(armed);
}

void libera_plugin_disconnect(void* handle) {
    delete static_cast<ExampleHandle*>(handle);
}

} // extern "C"
