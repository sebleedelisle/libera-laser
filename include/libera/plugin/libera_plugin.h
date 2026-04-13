/*
 * libera_plugin.h — Public C API for Libera controller plugins.
 *
 * This API is intentionally shaped like Libera's built-in backend model:
 *
 * - one plugin-wide "backend" object
 * - discover() on that backend
 * - connect_controller() returning one controller instance
 * - controller methods such as send_points(), set_armed(), and
 *   get_buffer_state()
 *
 * Plugins export one function:
 *
 *   const libera_plugin_api_t* libera_plugin_get_api(void);
 *
 * The returned table describes the plugin and provides the callbacks Libera
 * should use to drive it.
 */

#ifndef LIBERA_PLUGIN_H
#define LIBERA_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBERA_PLUGIN_API_VERSION 1

#define LIBERA_PLUGIN_MAX_ID 128
#define LIBERA_PLUGIN_MAX_LABEL 256
#define LIBERA_PLUGIN_MAX_IP 64
#define LIBERA_PLUGIN_MAX_CONNECT_COOKIE 64

typedef enum {
    LIBERA_OK                   = 0,
    LIBERA_ERR_DISCONNECTED     = 1,
    LIBERA_ERR_TIMEOUT          = 2,
    LIBERA_ERR_BUSY             = 3,
    LIBERA_ERR_PROTOCOL         = 4,
    LIBERA_ERR_INVALID_ARGUMENT = 5,
    LIBERA_ERR_INTERNAL         = 6
} libera_status_t;

typedef enum {
    LIBERA_LOG_INFO    = 0,
    LIBERA_LOG_WARNING = 1,
    LIBERA_LOG_ERROR   = 2
} libera_log_level_t;

typedef enum {
    LIBERA_CONTROLLER_USAGE_UNKNOWN = 0,
    LIBERA_CONTROLLER_USAGE_IDLE = 1,
    LIBERA_CONTROLLER_USAGE_ACTIVE = 2,
    LIBERA_CONTROLLER_USAGE_BUSY_EXCLUSIVE = 3
} libera_controller_usage_state_t;

/*
 * Normalised point format passed across the plugin boundary.
 */
typedef struct {
    int16_t  x;
    int16_t  y;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t i;
    uint16_t u1;
    uint16_t u2;
} libera_point_t;

typedef struct {
    int32_t points_in_buffer;
    int32_t total_buffer_points;
} libera_buffer_state_t;

/*
 * Opaque per-connection token passed by the host when a controller is opened.
 * Plugins must pass it back to host services when reporting latency/errors.
 */
typedef void* libera_host_ctx_t;

/*
 * Host callbacks exposed to plugins.
 */
typedef struct {
    uint32_t abi_version;

    void (*log)(libera_log_level_t level, const char* message);

    void (*record_latency)(libera_host_ctx_t host_ctx,
                           uint64_t nanoseconds);

    void (*report_error)(libera_host_ctx_t host_ctx,
                         const char* code,
                         const char* label);
} libera_host_services_t;

/*
 * Optional network metadata for discovery results.
 */
typedef struct {
    bool has_value;
    char ip[LIBERA_PLUGIN_MAX_IP];
    uint16_t port;
} libera_network_info_t;

/*
 * Discovery result.
 *
 * connect_cookie is copied back into connect_controller() so plugins can avoid
 * re-looking-up devices by string id if they already have a stable small token.
 */
typedef struct {
    char id[LIBERA_PLUGIN_MAX_ID];
    char label[LIBERA_PLUGIN_MAX_LABEL];
    uint32_t max_point_rate;

    libera_controller_usage_state_t usage_state;
    libera_network_info_t network;

    uint8_t connect_cookie[LIBERA_PLUGIN_MAX_CONNECT_COOKIE];
    uint32_t connect_cookie_size;
} libera_controller_info_t;

typedef void (*libera_emit_controller_fn)(void* ctx,
                                          const libera_controller_info_t* info);

/*
 * Property definitions are static. The host can implement property listing and
 * lookup-by-key from this table, so the plugin only needs to implement reading
 * by property index.
 */
typedef struct {
    const char* key;
    const char* label;
} libera_property_def_t;

/*
 * Main plugin API table.
 *
 * Required callbacks:
 * - discover
 * - connect_controller
 * - destroy_controller
 * - send_points
 *
 * Everything else is optional unless noted.
 */
typedef struct {
    uint32_t abi_version;

    /*
     * Stable backend identity.
     * type_name matches the role of ControllerInfo::type() in built-in
     * backends, for example "EtherDream" or "Helios".
     */
    const char* type_name;
    const char* display_name;

    /*
     * Optional plugin-wide lifetime.
     *
     * If create_backend is NULL, Libera passes backend = NULL to discover()
     * and connect_controller().
     *
     * If destroy_backend is NULL, Libera treats backend teardown as a no-op.
     */
    void* (*create_backend)(const libera_host_services_t* host);
    void  (*destroy_backend)(void* backend);

    /*
     * Optional: called before discover() when the host wants fresh state.
     * Useful for network discovery plugins.
     */
    void  (*rescan)(void* backend);

    /*
     * Required manager-like operations.
     */
    void  (*discover)(void* backend,
                      libera_emit_controller_fn emit,
                      void* ctx);

    void* (*connect_controller)(void* backend,
                                const libera_controller_info_t* info,
                                libera_host_ctx_t host_ctx);

    /*
     * Required controller-like operations.
     */
    void (*destroy_controller)(void* controller);

    libera_status_t (*send_points)(void* controller,
                                   const libera_point_t* points,
                                   uint32_t count);

    /*
     * Optional controller operations.
     */
    void (*set_point_rate)(void* controller, uint32_t point_rate);
    void (*set_armed)(void* controller, bool armed);
    int  (*get_buffer_state)(void* controller, libera_buffer_state_t* out);

    /*
     * Optional static property schema.
     *
     * If properties is NULL or property_count is 0, the plugin exposes no
     * properties.
     */
    const libera_property_def_t* properties;
    uint32_t property_count;

    /*
     * Optional property reader.
     *
     * The host resolves key -> property index itself from the properties
     * table. Return the number of bytes written or needed, excluding the
     * terminating null, or a negative value on failure.
     */
    int (*read_property)(void* controller,
                         uint32_t property_index,
                         char* out,
                         uint32_t out_size);
} libera_plugin_api_t;

/*
 * Single export every plugin must provide.
 */
const libera_plugin_api_t* libera_plugin_get_api(void);

/* ------------------------------------------------------------------ */
/* Helper utilities                                                    */
/* ------------------------------------------------------------------ */

static inline void libera_copy_string(char* dst,
                                      uint32_t dst_size,
                                      const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static inline void libera_controller_info_init(libera_controller_info_t* info,
                                               const char* id,
                                               const char* label,
                                               uint32_t max_point_rate) {
    if (!info) {
        return;
    }

    memset(info, 0, sizeof(*info));
    libera_copy_string(info->id, sizeof(info->id), id);
    libera_copy_string(info->label, sizeof(info->label), label);
    info->max_point_rate = max_point_rate;
    info->usage_state = LIBERA_CONTROLLER_USAGE_UNKNOWN;
}

static inline void libera_controller_info_set_network(libera_controller_info_t* info,
                                                      const char* ip,
                                                      uint16_t port) {
    if (!info) {
        return;
    }

    info->network.has_value = true;
    libera_copy_string(info->network.ip, sizeof(info->network.ip), ip);
    info->network.port = port;
}

static inline void libera_controller_info_set_cookie(libera_controller_info_t* info,
                                                     const void* data,
                                                     uint32_t size) {
    if (!info) {
        return;
    }

    if (!data || size == 0) {
        info->connect_cookie_size = 0;
        return;
    }

    if (size > LIBERA_PLUGIN_MAX_CONNECT_COOKIE) {
        size = LIBERA_PLUGIN_MAX_CONNECT_COOKIE;
    }

    memcpy(info->connect_cookie, data, size);
    info->connect_cookie_size = size;
}

#ifdef __cplusplus
#define LIBERA_PLUGIN_EXPORT(API_VAR) \
    extern "C" const libera_plugin_api_t* libera_plugin_get_api(void) { return &(API_VAR); }
#else
#define LIBERA_PLUGIN_EXPORT(API_VAR) \
    const libera_plugin_api_t* libera_plugin_get_api(void) { return &(API_VAR); }
#endif

#ifdef __cplusplus
}
#endif

#endif /* LIBERA_PLUGIN_H */
