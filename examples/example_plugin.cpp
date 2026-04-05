/*
 * example_plugin — minimal libera DAC plugin.
 * ============================================
 *
 * This file is a complete, working Libera plugin that you can use as a
 * starting point for supporting your own laser controller. It simulates a
 * single device that accepts and discards every point it is sent, so you
 * can verify that the whole load → discover → connect → stream → disconnect
 * cycle works end-to-end without any real hardware attached.
 *
 * To turn this into a real plugin you would:
 *   1. Replace the stub handle (`ExampleHandle`) with whatever state your
 *      hardware transport needs (USB handle, socket, SDK session, ...).
 *   2. Replace `libera_plugin_discover` with code that enumerates real
 *      devices on your transport.
 *   3. Replace `libera_plugin_connect` with code that actually opens a
 *      connection to the chosen device.
 *   4. Replace `libera_plugin_send_points` with code that pushes points to
 *      your hardware.
 *   5. Fill in `libera_plugin_get_buffer_state` if your hardware can tell
 *      you how full its buffer is (most can).
 *   6. Change the `type()` / `name()` strings to identify your plugin.
 *
 * The plugin API is pure C, so you can also port this to any language that
 * can produce a shared library with C linkage (Rust, Zig, D, ...).
 *
 * Build as a shared library and place the output in Libera's plugins
 * directory:
 *
 *   # macOS / Linux
 *   c++ -shared -fPIC -std=c++17 -o example-plugin.dylib example_plugin.cpp \
 *       -I ../include
 *
 *   # Windows (MSVC)
 *   cl /LD /std:c++17 /I ..\include example_plugin.cpp /Fe:example-plugin.dll
 */

// libera_plugin.h declares the C types and function signatures that every
// plugin must use. It has no dependencies on the rest of Libera — it is the
// entire ABI between the host and the plugin.
#include "libera/plugin/libera_plugin.h"

#include <atomic>
#include <cstring>

namespace {

/*
 * ExampleHandle — per-connection state.
 * -------------------------------------
 *
 * The host calls libera_plugin_connect() once per device it wants to use.
 * Whatever that function returns is treated by the host as an opaque
 * `void*` handle, and the host will pass it back to you on every later
 * call that concerns this specific connection (send_points, set_armed,
 * set_point_rate, get_buffer_state, disconnect).
 *
 * In a real plugin this struct would typically contain:
 *   - the USB device handle / socket / SDK session pointer,
 *   - a worker thread and the queue/ring-buffer it pulls from,
 *   - the current point rate and armed flag,
 *   - any error or reconnect state.
 *
 * The atomics below let us update `rate` and `armed` from the host's
 * streaming thread without taking a lock. Real plugins often need a mutex
 * somewhere as well, but keep the send_points hot path lock-free if you
 * can.
 */
struct ExampleHandle {
    // The host passes a per-connection token (`host_ctx`) to connect(). We
    // store it so we can hand it back when calling host services like
    // record_latency / report_error. It is only valid between connect()
    // and disconnect() for this connection.
    libera_host_ctx_t host_ctx = nullptr;

    // Desired output point rate (points per second). Updated via
    // libera_plugin_set_point_rate() — can change at any time.
    std::atomic<uint32_t> rate{30000};

    // Whether the controller is currently allowed to emit light. Toggled
    // via libera_plugin_set_armed(). When false, a real plugin should
    // still accept points (so the stream doesn't stall) but output black
    // or mute colour channels on the wire.
    std::atomic<bool> armed{false};
};

/*
 * g_host — host services struct, provided at init() time.
 * -------------------------------------------------------
 *
 * The host fills this struct with function pointers we can call for
 * logging, latency reporting, and structured error reporting. It's
 * global here because it's the same for every connection the plugin
 * opens. The pointer itself (once set) remains valid for the whole
 * lifetime of the plugin, until libera_plugin_shutdown() is called.
 *
 * Individual function-pointer fields (log, record_latency, report_error)
 * may be NULL, so always null-check before calling them.
 */
const libera_host_services_t* g_host = nullptr;

// Tiny convenience wrapper: log only if the host supplied a log function.
// A real plugin will probably want more of these (hostReportError,
// hostRecordLatency, ...).
void hostLog(libera_log_level_t level, const char* message) {
    if (g_host && g_host->log) g_host->log(level, message);
}

} // anonymous namespace

// Everything the plugin exports must be in extern "C" so the host (and
// the host's dynamic loader) can find the symbols by name without any C++
// name-mangling getting in the way.
extern "C" {

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

/*
 * libera_plugin_api_version — first thing the host checks after loading.
 *
 * Return the value of LIBERA_PLUGIN_API_VERSION that was defined in the
 * libera_plugin.h you built against. If the host's idea of the ABI
 * version doesn't match, it will unload your plugin without calling
 * anything else. That's a feature, not a bug — it stops you from
 * crashing the host when the interface changes incompatibly.
 */
uint32_t libera_plugin_api_version(void) {
    return LIBERA_PLUGIN_API_VERSION;
}

/*
 * libera_plugin_type — short machine-readable tag for this plugin.
 *
 * Must be unique across all loaded plugins. The host uses it to route
 * connect() calls to the right plugin when the application asks to
 * connect to a specific ControllerInfo. Built-in examples include
 * "EtherDream" and "Helios". Pick something distinct and stable — it
 * may end up saved in user config files.
 */
const char* libera_plugin_type(void) {
    return "Example";
}

/*
 * libera_plugin_name — human-readable name for UI.
 *
 * Shown to end users in lists and logs. Can be changed between versions
 * without breaking anything.
 */
const char* libera_plugin_name(void) {
    return "Example Plugin";
}

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */
/* ------------------------------------------------------------------ */

/*
 * libera_plugin_init — called once, right after the shared library is
 * loaded and the version check has passed.
 *
 * Do any one-off setup here:
 *   - initialise the vendor SDK,
 *   - start a libusb context,
 *   - open a UDP socket for device discovery,
 *   - spin up a background discovery thread (if enumeration is slow),
 *   - etc.
 *
 * Store the host services pointer (or just the fields you care about)
 * so you can use them later. The pointer remains valid until shutdown().
 *
 * Return 0 on success, non-zero on failure. If you return non-zero, the
 * host will unload the plugin and not call anything else on it.
 */
int libera_plugin_init(const libera_host_services_t* host) {
    g_host = host;
    hostLog(LIBERA_LOG_INFO, "Example plugin initialised");
    return 0;
}

/*
 * libera_plugin_shutdown — called once, just before the library is
 * unloaded.
 *
 * Tear down anything init() started. The host guarantees that every
 * open connection has been closed via libera_plugin_disconnect() before
 * shutdown() is called, so you do not need to clean up per-connection
 * state here.
 *
 * After this returns, the host will not call any further functions on
 * this plugin, and the library may be unloaded from memory, so make
 * sure any background threads you started have actually joined.
 */
void libera_plugin_shutdown(void) {
    // Nothing to tear down in this stub.
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

/*
 * libera_plugin_discover — enumerate every device currently visible.
 *
 * The host calls this whenever the application asks "what controllers
 * are available?". It can be called repeatedly, from a discovery
 * thread, so keep it reasonably quick (a few ms at most). If your
 * enumeration is slow, cache the last result in init() and refresh it
 * in the background.
 *
 * For each device you can see, fill a libera_controller_info_t and
 * call emit(ctx, &info). The host copies the struct's contents inside
 * emit(), so it is safe to put `info` on the stack and reuse it for
 * the next device.
 *
 * Fields to fill:
 *   - id:            a unique, STABLE identifier for this device. The
 *                    host stores this and later passes it to connect(),
 *                    so it must match between discover/connect calls
 *                    and ideally survive across app restarts. Serial
 *                    numbers, MAC addresses, or stable bus paths are
 *                    all good choices.
 *   - label:         human-friendly name shown to the user.
 *   - maxPointRate:  the highest points-per-second this device can
 *                    sustain. The host will never ask for a higher
 *                    rate than this.
 *
 * Emitting zero devices is perfectly legal — it just means there's
 * nothing to talk to right now.
 */
void libera_plugin_discover(libera_emit_controller_fn emit, void* ctx) {
    libera_controller_info_t info{};

    // strncpy with sizeof-1 leaves the final byte as the zero the `{}`
    // initialiser gave us, so the strings are always null-terminated
    // even if we tried to copy something too long.
    std::strncpy(info.id,    "example-dac-1", sizeof(info.id) - 1);
    std::strncpy(info.label, "Example DAC",   sizeof(info.label) - 1);
    info.maxPointRate = 30000;

    emit(ctx, &info);

    // If you had multiple devices, you would fill info again and call
    // emit(ctx, &info) once per device.
}

/* ------------------------------------------------------------------ */
/* Per-connection lifecycle                                            */
/* ------------------------------------------------------------------ */

/*
 * libera_plugin_connect — open a connection to a specific device.
 *
 * `controller_id` matches the `id` field from a previous discover()
 * emission. Use it to look up which of your devices the host wants.
 *
 * `host_ctx` is an opaque per-connection token the host gives you.
 * Remember it — you will pass it back when calling host services
 * (record_latency, report_error) so the host knows which connection
 * the call is about. Do NOT use this value after disconnect() returns
 * for this connection.
 *
 * Return an opaque handle on success (any non-NULL pointer you like —
 * usually a pointer to your per-connection state struct), or NULL on
 * failure. The host will not call any other per-connection functions
 * until connect() has succeeded.
 *
 * A real plugin would also:
 *   - actually open the USB device / socket / SDK session here,
 *   - start a worker thread if it needs one,
 *   - push an initial "safe" state (lasers off, zero intensity),
 *   - report errors via g_host->report_error() if anything goes wrong.
 */
void* libera_plugin_connect(const char* controller_id,
                            libera_host_ctx_t host_ctx) {
    // Defensive: only connect if the id is one of ours. If you have
    // many devices you'd look this up in a table instead.
    if (std::strcmp(controller_id, "example-dac-1") != 0) {
        return nullptr;
    }
    auto* h = new ExampleHandle;
    h->host_ctx = host_ctx;
    return h;
}

/*
 * libera_plugin_set_point_rate — host tells you the desired output rate.
 *
 * Called at least once after connect(), and again any time the user
 * (or the host) changes the configured rate. Apply it promptly to your
 * transport. If your hardware doesn't support arbitrary rates, clamp
 * or round as appropriate.
 *
 * Called on the host's control thread — don't block here. If applying
 * the rate requires a round-trip to the hardware, queue it for your
 * worker thread instead of doing it inline.
 */
void libera_plugin_set_point_rate(void* handle, uint32_t point_rate) {
    auto* h = static_cast<ExampleHandle*>(handle);
    if (h) h->rate.store(point_rate);
}

/* ------------------------------------------------------------------ */
/* The hot path                                                        */
/* ------------------------------------------------------------------ */

/*
 * libera_plugin_send_points — the streaming hot path.
 *
 * Called from the host's per-controller streaming thread, many times
 * per second, as long as the connection is alive. Each call hands you
 * a batch of points to get out to the hardware. Batch sizes vary.
 *
 * The point format is fixed (libera_point_t, see libera_plugin.h):
 *   x, y    — signed 16-bit, full scan range (-32768 .. 32767)
 *   r, g, b — unsigned 16-bit (0 .. 65535)
 *   i       — unsigned 16-bit master intensity (0 .. 65535)
 *
 * If your hardware expects a different format (e.g. 12-bit values,
 * float colours, a different channel order), convert here.
 *
 * Performance matters: this runs at kHz rates. Avoid per-call heap
 * allocation. Keep preallocated buffers on the handle and reuse them.
 *
 * Returning anything other than LIBERA_OK tells the host something
 * went wrong. The host may decide to report the error, back off, or
 * reconnect depending on the error code.
 */
libera_status_t libera_plugin_send_points(void*                 handle,
                                          const libera_point_t* /*points*/,
                                          uint32_t              /*count*/) {
    auto* h = static_cast<ExampleHandle*>(handle);
    if (!h) return LIBERA_ERR_INVALID_ARGUMENT;

    // A real plugin would:
    //   1. Convert each libera_point_t into the hardware's native
    //      point format.
    //   2. Copy/push into a ring buffer shared with a worker thread,
    //      OR write directly to the transport if it is already
    //      asynchronous.
    //   3. If `h->armed` is false, blank the colour channels before
    //      sending so the laser stays dark.
    //   4. Optionally call g_host->record_latency(h->host_ctx, ns)
    //      with the time it took to hand the batch to the transport,
    //      so the host can surface latency percentiles in its UI.
    //   5. Return LIBERA_ERR_DISCONNECTED / LIBERA_ERR_TIMEOUT / etc.
    //      if the transport misbehaves.

    return LIBERA_OK;
}

/*
 * libera_plugin_get_buffer_state — report how full the device buffer is.
 *
 * The host uses this to pace send_points() calls. If the device can
 * tell you how many points it currently has queued, fill both fields
 * and return 0. If you only know one of them, set the other to -1.
 * If you have no idea, set both to -1 and return non-zero — the host
 * will fall back to a fixed cadence.
 *
 * Accurate buffer reporting lets the host run the device at the edge
 * of its buffer capacity without under-running, so it is well worth
 * implementing properly if your hardware gives you the numbers.
 */
int libera_plugin_get_buffer_state(void* /*handle*/, libera_buffer_state_t* out) {
    if (!out) return -1;
    // Buffer state is not available for this simulated DAC.
    out->points_in_buffer = -1;
    out->total_buffer_points = -1;
    return -1;
}

/*
 * libera_plugin_set_armed — enable or disable light output.
 *
 * The user (or the application's safety logic) flips this. When
 * armed == false your plugin MUST guarantee the laser is dark,
 * regardless of what points are being streamed. The safest and
 * simplest implementation is: keep accepting and sending points as
 * normal, but force r/g/b/i to 0 on the wire whenever armed is false.
 *
 * Do not take this as a hint — treat it as a hard safety contract.
 */
void libera_plugin_set_armed(void* handle, bool armed) {
    auto* h = static_cast<ExampleHandle*>(handle);
    if (h) h->armed.store(armed);
}

/*
 * libera_plugin_disconnect — close a connection.
 *
 * Stop every piece of per-connection activity (worker threads,
 * pending transport operations, timers) BEFORE this function returns.
 * After disconnect() returns, the host may invalidate host_ctx, and
 * calling any host service with it is undefined behaviour.
 *
 * Free the handle and anything it owns. After this returns, `handle`
 * must not be used again.
 */
void libera_plugin_disconnect(void* handle) {
    delete static_cast<ExampleHandle*>(handle);
}

/* ------------------------------------------------------------------ */
/* Optional exports                                                    */
/* ------------------------------------------------------------------ */

/*
 * libera_plugin_list_properties — enumerate per-device properties.
 *
 * This is an OPTIONAL export. If your plugin doesn't have anything
 * interesting to show in a "device info" panel (firmware version,
 * serial number, IP address, temperature, ...), just don't export
 * this function and the host will treat the device as having no
 * properties.
 *
 * For each property you want to expose, call emit(ctx, key, label):
 *   key   — short machine tag, stable across releases (e.g. "firmware").
 *   label — human-friendly display name (e.g. "Firmware version").
 *
 * Per-device: different devices on the same plugin may legitimately
 * report different property sets (e.g. network devices have "ip",
 * USB devices don't).
 */
void libera_plugin_list_properties(void* /*handle*/,
                                   libera_emit_property_fn emit, void* ctx) {
    emit(ctx, "firmware", "Firmware version");
    emit(ctx, "serial",   "Serial number");
}

/*
 * libera_plugin_get_property — read a property as a null-terminated string.
 *
 * This is an OPTIONAL export. Paired with list_properties — if you
 * export one you almost certainly want to export the other.
 *
 * Semantics match snprintf():
 *   - Write a null-terminated value into `out` (up to `out_size` bytes).
 *   - Return the number of bytes that WOULD have been written if the
 *     buffer were large enough, excluding the trailing null.
 *   - Return a negative value if the key is unknown or unreadable.
 *
 * The host calls this with a ~256 byte stack buffer first, then retries
 * with a larger heap buffer if truncation is detected, so short values
 * need no extra work on your side.
 *
 * Values are always strings. Format numbers ("42.5°C"), versions
 * ("1.2.3"), or anything else as text here.
 */
int libera_plugin_get_property(void* /*handle*/,
                               const char* key,
                               char* out, uint32_t out_size) {
    const char* value = nullptr;
    if      (std::strcmp(key, "firmware") == 0) value = "0.1.0";
    else if (std::strcmp(key, "serial")   == 0) value = "EXAMPLE-0001";
    else return -1;

    const int needed = static_cast<int>(std::strlen(value));
    if (out && out_size > 0) {
        const uint32_t copy = (static_cast<uint32_t>(needed) < out_size - 1)
            ? static_cast<uint32_t>(needed) : out_size - 1;
        std::memcpy(out, value, copy);
        out[copy] = '\0';
    }
    return needed;
}

} // extern "C"
