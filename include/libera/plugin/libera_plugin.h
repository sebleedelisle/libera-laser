/*
 * libera_plugin.h — Public C API for libera DAC plugins.
 *
 * ⚠️  WORK IN PROGRESS — PROOF OF CONCEPT.
 *     The plugin ABI is not stable yet.  Function signatures, host services,
 *     error codes, and lifecycle behaviour WILL change in future releases.
 *     Rebuild your plugin against the current header whenever you update
 *     libera.  Do not rely on this interface for production work yet.
 *
 * Plugin authors: implement all libera_plugin_* functions below and compile
 * your code into a shared library (.dylib / .so / .dll).  Place the resulting
 * file in the libera plugins directory and it will be loaded automatically.
 *
 * The ABI is plain C so any language/compiler that can produce a shared
 * library with C linkage will work.
 *
 * Model: pure streaming.  The host drives a streaming loop per connected
 * controller and pushes batches of points at the rate it decides.  Plugins
 * that aggregate (e.g. frame-based SDKs) should buffer internally and pace
 * themselves using their own worker thread, reporting back-pressure via
 * libera_plugin_get_buffer_state().
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
    char     id[128];       /* Unique, stable controller identifier.    */
    char     label[256];    /* Human-readable display name.             */
    uint32_t maxPointRate;  /* Maximum supported point rate (pts/sec).  */
} libera_controller_info_t;

/*
 * Normalised laser point passed across the plugin boundary.
 *
 *   x, y   — signed 16-bit, full range (-32768 .. 32767)
 *   r,g,b  — unsigned 16-bit (0 .. 65535)
 *   i      — unsigned 16-bit master intensity (0 .. 65535)
 *   u1, u2 — unsigned 16-bit user channels (0 .. 65535) for plugins whose
 *            hardware supports extra outputs (safety mask, waveform, etc.)
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

/*
 * Buffer fullness reported by a plugin-managed controller.
 *
 *   points_in_buffer    — points currently waiting to be output, or -1 if
 *                          unknown.
 *   total_buffer_points — total buffer capacity in points, or -1 if unknown.
 */
typedef struct {
    int32_t points_in_buffer;
    int32_t total_buffer_points;
} libera_buffer_state_t;

/*
 * Status code returned by libera_plugin_send_points().  Plugins can add
 * their own codes >= 100 if needed; the host treats unknown non-zero codes
 * as "internal".
 */
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

/* ------------------------------------------------------------------ */
/* Host services                                                       */
/* ------------------------------------------------------------------ */

/*
 * Opaque per-connection handle passed by the host at connect() time.  The
 * plugin echoes it back when calling host services so the host knows which
 * controller a call belongs to.  The value is only valid between connect()
 * and the matching disconnect() on the same controller.
 */
typedef void* libera_host_ctx_t;

/*
 * Services exposed by the host to every loaded plugin.  Passed to init().
 *
 * This struct is designed to grow: new fields may be appended at the end
 * in future releases.  Plugins must check `abi_version` and, if they only
 * need a subset of services, ignore fields they do not recognise.
 */
#define LIBERA_HOST_SERVICES_VERSION 1

typedef struct {
    /* = LIBERA_HOST_SERVICES_VERSION at the time this struct was filled. */
    uint32_t abi_version;

    /*
     * Log a message through the host's logging system.  NULL-safe: if the
     * field is NULL the plugin should silently skip logging.  The plugin
     * must NOT store the message pointer — copy it if needed.
     */
    void (*log)(libera_log_level_t level, const char* message);

    /*
     * Report a transport/admission latency sample for a specific connection.
     * Feeds the host's rolling percentile stats.  `host_ctx` must be the
     * value passed to connect() for that controller.  May be NULL if host
     * does not accept latency samples.
     */
    void (*record_latency)(libera_host_ctx_t host_ctx, uint64_t nanoseconds);

    /*
     * Report a structured intermittent error.  `code` is a short machine
     * tag (e.g. "usb.timeout"), `label` is human-readable.  `host_ctx`
     * identifies the connection.  May be NULL.
     */
    void (*report_error)(libera_host_ctx_t host_ctx,
                         const char* code,
                         const char* label);
} libera_host_services_t;

/* ------------------------------------------------------------------ */
/* Device discovery callback                                           */
/* ------------------------------------------------------------------ */

/*
 * Callback invoked by the host inside libera_plugin_discover() once per
 * device.  The plugin fills a `libera_controller_info_t` on its stack and
 * calls `emit(ctx, &info)`.  The host copies the struct contents before
 * the callback returns — the plugin does not need to keep the memory
 * alive afterwards.
 */
typedef void (*libera_emit_controller_fn)(void* ctx,
                                          const libera_controller_info_t* info);

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
 *
 * `host` points to a host services struct that remains valid for the
 * lifetime of the plugin (until shutdown() is called).  The plugin may
 * store the pointer (or copy individual fields) and use the services
 * from any thread.  `host` itself will not be NULL, but individual
 * function-pointer fields may be — guard each one before calling.
 *
 * Return 0 on success, non-zero on failure.
 */
int libera_plugin_init(const libera_host_services_t* host);

/*
 * Called once before the shared library is unloaded.
 * Tear down any global SDK state.  The host will have already called
 * disconnect() for every open controller before this.
 */
void libera_plugin_shutdown(void);

/*
 * Discover available controllers.
 *
 * The plugin must call `emit(ctx, &info)` exactly once per controller.
 * No upper bound on the number of emissions.  It is legal to emit zero
 * controllers.
 */
void libera_plugin_discover(libera_emit_controller_fn emit, void* ctx);

/*
 * Open a connection to the controller identified by `controller_id`
 * (matches the `id` field from a previous discover() emission).
 *
 * `host_ctx` is an opaque host identifier the plugin must remember and
 * pass back to host services (record_latency, report_error) associated
 * with this connection.  The plugin must stop using `host_ctx` before
 * disconnect() returns.
 *
 * Return an opaque plugin handle on success, or NULL on failure.
 */
void* libera_plugin_connect(const char* controller_id,
                            libera_host_ctx_t host_ctx);

/*
 * Inform the plugin of the current desired output point rate.  Called
 * at least once after connect() and again any time the host's configured
 * rate changes.  The plugin should apply it to its transport promptly.
 */
void libera_plugin_set_point_rate(void* handle, uint32_t point_rate);

/*
 * Send a batch of points to the controller.
 *
 * The plugin is responsible for any internal buffering/pacing it needs
 * in order to accept variable-sized batches at the host's streaming
 * cadence.  Return LIBERA_OK on success or a libera_status_t error code.
 */
libera_status_t libera_plugin_send_points(void*                 handle,
                                          const libera_point_t* points,
                                          uint32_t              count);

/*
 * Fill `out` with current buffer state.  Either field may be set to -1
 * if unknown.  Return 0 on success, non-zero if no information is
 * available at all.
 */
int libera_plugin_get_buffer_state(void* handle, libera_buffer_state_t* out);

/*
 * Enable or disable laser output.
 */
void libera_plugin_set_armed(void* handle, bool armed);

/*
 * Disconnect from the controller and free any per-connection resources.
 * The plugin must stop all background work associated with this handle
 * and stop calling host services with its `host_ctx` before returning.
 * The handle must not be used after this call.
 */
void libera_plugin_disconnect(void* handle);

/* ------------------------------------------------------------------ */
/* Optional exports                                                     */
/*                                                                      */
/* The functions below are optional: plugins may omit them entirely.    */
/* The host resolves each symbol non-fatally and treats a missing       */
/* function as "this plugin does not provide that capability".          */
/* ------------------------------------------------------------------ */

/*
 * Hint for network-discovery plugins: drop any cached state and
 * re-enumerate devices.  Called from the host's discovery thread.
 */
void libera_plugin_rescan(void);

/*
 * Per-device property enumeration callback.
 *
 *   key   — short machine-readable identifier (e.g. "firmware").
 *   label — human-readable display name (e.g. "Firmware version").
 *
 * The plugin calls this once per available property.  The strings are
 * copied by the host before emit() returns — the plugin does not need
 * to keep them alive afterwards.
 */
typedef void (*libera_emit_property_fn)(void* ctx,
                                        const char* key,
                                        const char* label);

/*
 * List the property keys this connection exposes.  The plugin may emit
 * zero or more entries.  Plugins that do not expose properties may
 * simply not export this function.
 */
void libera_plugin_list_properties(void* handle,
                                   libera_emit_property_fn emit,
                                   void* ctx);

/*
 * Read a property value as a null-terminated string into a host-owned
 * buffer.
 *
 * Returns the number of bytes that would have been written if `out_size`
 * were large enough (excluding the trailing null) — matching snprintf()
 * semantics — or a negative value if `key` is unknown or the property
 * cannot be read.  If `out_size` is too small the plugin still writes a
 * truncated, null-terminated value so hosts that do not grow-and-retry
 * see something usable.
 *
 * `out` may be NULL if `out_size` is 0 (query-size mode).
 */
int libera_plugin_get_property(void* handle,
                               const char* key,
                               char* out,
                               uint32_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* LIBERA_PLUGIN_H */
