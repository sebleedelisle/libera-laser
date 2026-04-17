/*
 * example_plugin — minimal Libera controller plugin.
 *
 * This file is a complete, working plugin built against libera_plugin.h.
 * It simulates one controller that accepts and discards every submission.
 *
 * The plugin API now mirrors Libera's built-in backend structure:
 *
 * - one plugin-wide backend object
 * - discover() on that backend
 * - connect_controller() returning one controller handle per live connection
 * - controller methods such as send_points(), send_frame(), set_armed(), and
 *   get_buffer_state()
 *
 * This example exposes both point-ingester and frame-ingester callbacks so
 * plugin authors can compare the two shapes in one place. Current hosts prefer
 * the frame callbacks when both are present.
 *
 * Build as a shared library and place the output in one of Libera's default
 * plugin directories, such as plugins/ next to the executable. Hosts can still
 * override or extend the search path with System::setPluginDirectory() /
 * System::addPluginDirectory() when they want custom locations:
 *
 *   # macOS
 *   c++ -shared -fPIC -std=c++17 -o example-plugin.dylib example_plugin.cpp \
 *       -I ../include
 *
 *   # Linux
 *   c++ -shared -fPIC -std=c++17 -o example-plugin.so example_plugin.cpp \
 *       -I ../include
 *
 *   # Windows (MSVC)
 *   cl /LD /std:c++17 /I ..\include example_plugin.cpp /Fe:example-plugin.dll
 */

#include "libera/plugin/libera_plugin.h"

#include <atomic>
#include <cstring>

namespace {

struct ExampleBackend {
    const libera_host_services_t* host = nullptr;
};

struct ExampleController {
    ExampleBackend* backend = nullptr;
    libera_host_ctx_t hostCtx = nullptr;
    std::atomic<uint32_t> pointRate{30000};
    std::atomic<bool> armed{false};
};

libera_status_t acceptSubmission(ExampleController* controller,
                                 const libera_point_t* points,
                                 uint32_t count) {
    if (!controller || (!points && count > 0)) {
        return LIBERA_ERR_INVALID_ARGUMENT;
    }

    // A real plugin would convert and forward the points to hardware here.
    // The example just accepts the submission and optionally records a tiny
    // fake latency so plugin authors can see how to call back into the host.
    if (controller->backend &&
        controller->backend->host &&
        controller->backend->host->record_latency) {
        controller->backend->host->record_latency(controller->hostCtx, 1000);
    }

    (void)points;
    (void)count;
    return LIBERA_OK;
}

void hostLog(ExampleBackend* backend,
             libera_log_level_t level,
             const char* message) {
    if (backend && backend->host && backend->host->log) {
        backend->host->log(level, message);
    }
}

void* createBackend(const libera_host_services_t* host) {
    auto* backend = new ExampleBackend;
    backend->host = host;
    hostLog(backend, LIBERA_LOG_INFO, "Example plugin backend created");
    return backend;
}

void destroyBackend(void* rawBackend) {
    auto* backend = static_cast<ExampleBackend*>(rawBackend);
    if (!backend) {
        return;
    }

    hostLog(backend, LIBERA_LOG_INFO, "Example plugin backend destroyed");
    delete backend;
}

void discover(void* rawBackend,
              libera_emit_controller_fn emit,
              void* ctx) {
    auto* backend = static_cast<ExampleBackend*>(rawBackend);
    if (!emit) {
        return;
    }

    libera_controller_info_t info;
    libera_controller_info_init(&info, "example-dac-1", "Example DAC", 30000);
    info.usage_state = LIBERA_CONTROLLER_USAGE_IDLE;

    // The cookie is copied back into connect_controller(). For real hardware
    // this is a good place to stash a small index or transport handle key.
    const uint32_t deviceIndex = 0;
    libera_controller_info_set_cookie(&info, &deviceIndex, sizeof(deviceIndex));

    emit(ctx, &info);

    hostLog(backend, LIBERA_LOG_INFO, "Example plugin discover() emitted one controller");
}

void* connectController(void* rawBackend,
                        const libera_controller_info_t* info,
                        libera_host_ctx_t hostCtx) {
    auto* backend = static_cast<ExampleBackend*>(rawBackend);
    if (!info || std::strcmp(info->id, "example-dac-1") != 0) {
        return nullptr;
    }

    auto* controller = new ExampleController;
    controller->backend = backend;
    controller->hostCtx = hostCtx;
    return controller;
}

void destroyController(void* rawController) {
    auto* controller = static_cast<ExampleController*>(rawController);
    delete controller;
}

void setPointRate(void* rawController, uint32_t pointRate) {
    auto* controller = static_cast<ExampleController*>(rawController);
    if (!controller) {
        return;
    }
    controller->pointRate.store(pointRate);
}

void setArmed(void* rawController, bool armed) {
    auto* controller = static_cast<ExampleController*>(rawController);
    if (!controller) {
        return;
    }
    controller->armed.store(armed);
}

libera_status_t sendPoints(void* rawController,
                           const libera_point_t* points,
                           uint32_t count) {
    auto* controller = static_cast<ExampleController*>(rawController);
    return acceptSubmission(controller, points, count);
}

libera_status_t getFrameRequirements(void* rawController,
                                     libera_frame_requirements_t* out) {
    auto* controller = static_cast<ExampleController*>(rawController);
    if (!controller || !out) {
        return LIBERA_ERR_INVALID_ARGUMENT;
    }

    // Frame-ingester plugins tell the host what one natural hardware frame
    // looks like. The host then adapts either content source into that shape.
    libera_frame_requirements_init(out,
                                   /* maximumPointsRequired */ 300,
                                   /* preferredPointCount   */ 300,
                                   /* blankFramePointCount  */ 300);
    out->estimated_first_point_render_delay_ns = 5ull * 1000ull * 1000ull;
    return LIBERA_OK;
}

libera_status_t sendFrame(void* rawController,
                          const libera_point_t* points,
                          uint32_t count) {
    auto* controller = static_cast<ExampleController*>(rawController);
    return acceptSubmission(controller, points, count);
}

int getBufferState(void* rawController, libera_buffer_state_t* out) {
    auto* controller = static_cast<ExampleController*>(rawController);
    if (!controller || !out) {
        return -1;
    }

    // This fake device reports an always-empty 10 ms buffer at 30 kpps.
    out->points_in_buffer = 0;
    out->total_buffer_points = 300;
    return 0;
}

const libera_property_def_t exampleProperties[] = {
    {"serial", "Serial"},
    {"transport", "Transport"},
};

int readProperty(void* rawController,
                 uint32_t propertyIndex,
                 char* out,
                 uint32_t outSize) {
    auto* controller = static_cast<ExampleController*>(rawController);
    if (!controller) {
        return -1;
    }

    const char* value = nullptr;
    switch (propertyIndex) {
        case 0:
            value = "example-dac-1";
            break;
        case 1:
            value = "simulated";
            break;
        default:
            return -1;
    }

    const int length = static_cast<int>(std::strlen(value));
    if (out && outSize > 0) {
        libera_copy_string(out, outSize, value);
    }
    return length;
}

const libera_plugin_api_t examplePluginApi = {
    /* abi_version        */ LIBERA_PLUGIN_API_VERSION,
    /* type_name          */ "Example",
    /* display_name       */ "Example Plugin",
    /* create_backend     */ &createBackend,
    /* destroy_backend    */ &destroyBackend,
    /* rescan             */ nullptr,
    /* discover           */ &discover,
    /* connect_controller */ &connectController,
    /* destroy_controller */ &destroyController,
    /* send_points        */ &sendPoints,
    /* set_point_rate     */ &setPointRate,
    /* set_armed          */ &setArmed,
    /* get_buffer_state   */ &getBufferState,
    /* properties         */ exampleProperties,
    /* property_count     */ 2,
    /* read_property      */ &readProperty,
    /* get_frame_requirements */ &getFrameRequirements,
    /* send_frame             */ &sendFrame,
};

} // namespace

LIBERA_PLUGIN_EXPORT(examplePluginApi)
