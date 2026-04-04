/*
 * libera_plugin.h — Public C API for libera DAC plugins.
 *
 * Plugin authors: implement all libera_plugin_* functions below and compile
 * your code into a shared library (.dylib / .so / .dll).  Place the resulting
 * file in the libera plugins directory and it will be loaded automatically.
 *
 * The ABI is plain C so any language/compiler that can produce a shared
 * library with C linkage will work.
 */

#ifndef LIBERA_PLUGIN_H
#define LIBERA_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump this when the plugin interface changes in an incompatible way. */
#define LIBERA_PLUGIN_API_VERSION 1

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char id[128];           /* Unique, stable controller identifier.    */
    char label[256];        /* Human-readable display name.             */
    uint32_t maxPointRate;  /* Maximum supported point rate (pts/sec).  */
} libera_controller_info_t;

/*
 * Normalised laser point passed across the plugin boundary.
 *
 *   x, y  — signed 16-bit, full range (-32768 .. 32767)
 *   r,g,b — unsigned 16-bit (0 .. 65535)
 *   i     — unsigned 16-bit master intensity (0 .. 65535)
 */
typedef struct {
    int16_t  x;
    int16_t  y;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t i;
} libera_point_t;

/* ------------------------------------------------------------------ */
/* Functions every plugin must export                                   */
/* ------------------------------------------------------------------ */

/*
 * Return LIBERA_PLUGIN_API_VERSION so the host can reject incompatible
 * plugins at load time.
 */
uint32_t libera_plugin_api_version(void);

/*
 * Short type tag used to route connectController() calls.
 * Must be unique across all loaded plugins (e.g. "EtherDream").
 */
const char* libera_plugin_type(void);

/*
 * Human-readable plugin name shown in UI (e.g. "Ether Dream").
 */
const char* libera_plugin_name(void);

/*
 * Called once after the shared library is loaded.
 * Perform any global SDK initialisation here.
 * Return 0 on success, non-zero on failure.
 */
int libera_plugin_init(void);

/*
 * Called once before the shared library is unloaded.
 * Tear down any global SDK state.
 */
void libera_plugin_shutdown(void);

/*
 * Discover available controllers.
 *
 * Fill `out` with up to `max_count` entries.  Return the number of
 * controllers written, or a negative value on error.
 */
int libera_plugin_discover(libera_controller_info_t* out, int max_count);

/*
 * Open a connection to the controller identified by `controller_id`
 * (matches the `id` field from a previous discover() call).
 *
 * Return an opaque handle on success, or NULL on failure.
 */
void* libera_plugin_connect(const char* controller_id);

/*
 * Send a batch of points to the controller.
 *
 * `point_rate` is the desired output rate in points per second.
 * Return 0 on success, non-zero on error.
 */
int libera_plugin_send_points(void*                  handle,
                              const libera_point_t*  points,
                              uint32_t               count,
                              uint32_t               point_rate);

/*
 * Return the controller's buffer fullness as a percentage (0–100),
 * or -1 if the information is not available.
 */
int libera_plugin_get_buffer_fullness(void* handle);

/*
 * Enable or disable laser output.
 */
void libera_plugin_set_armed(void* handle, bool armed);

/*
 * Disconnect from the controller and free any per-connection resources.
 * The handle must not be used after this call.
 */
void libera_plugin_disconnect(void* handle);

#ifdef __cplusplus
}
#endif

#endif /* LIBERA_PLUGIN_H */
