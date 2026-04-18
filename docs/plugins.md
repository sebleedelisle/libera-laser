# Writing a Libera plugin

Libera plugins are shared libraries that add controller support without having
to fork or rebuild Libera itself.

The plugin API is plain C, but it is deliberately shaped like Libera's built-in
backend model:

- one plugin-wide **backend** object
- `discover()` on that backend
- `connect_controller()` returning one **controller handle** per live connection
- controller methods such as `send_points()`, `send_frame()`, `set_armed()`, and
  `get_buffer_state()`

That keeps translating an in-tree backend into a plugin mostly mechanical.

## The one export

Every plugin exports exactly one symbol:

```c
const libera_plugin_api_t* libera_plugin_get_api(void);
```

That function returns a static `libera_plugin_api_t` describing the plugin and
the callbacks Libera should use.

The public header is:

- [libera_plugin.h](../include/libera/plugin/libera_plugin.h)

## Mental model

Think of the plugin as two layers:

1. **Backend**
   Owns plugin-wide state such as a vendor SDK context, USB/libusb setup, or a
   cached discovery list.
2. **Controller**
   Owns one live connection to one discovered device.

That maps directly onto Libera's built-in structure:

- built-in `AbstractControllerManager` / `ControllerManagerBase` ~= plugin backend callbacks
- built-in `LaserController` ~= plugin controller handle plus controller callbacks

## Loading plugin libraries

In the normal case you do not need to configure plugin directories at all.
Libera already searches:

- `plugins/` next to the executable
- `../plugins/` relative to the executable, which suits `bin/` + `plugins/`
  layouts
- the same relative paths from the current working directory

So the simplest setup is just:

```cpp
libera::System liberaSystem;
```

Only configure plugin search paths if you want to override those defaults:

```cpp
libera::System::setPluginDirectory("plugins");
libera::System::addPluginDirectory("/absolute/path/to/more/plugins");

libera::System liberaSystem;
```

Passing an empty string to `setPluginDirectory("")` disables plugin loading
entirely for that process.

If you never call `setPluginDirectory()`, Libera uses `LIBERA_PLUGIN_DIR` when
that environment variable is set. `LIBERA_PLUGIN_DIR` can contain more than one
directory, separated by `:` on POSIX and `;` on Windows.

There is not a first-class "plugin installer" in Libera itself today. Installing
a plugin currently just means placing the shared library in one of those
searched directories.

## Required callbacks

These fields in `libera_plugin_api_t` are required:

- `type_name`
- `display_name`
- `discover`
- `connect_controller`
- `destroy_controller`
- transport callback(s): `send_points`, or `get_frame_requirements` + `send_frame`

Everything else is optional.

Set `.abi_version = LIBERA_PLUGIN_API_VERSION`. The current unreleased plugin
API version is `1`.

## Optional callbacks

These can be omitted by setting the field to `NULL`:

- `create_backend`
- `destroy_backend`
- `rescan`
- `set_point_rate`
- `set_armed`
- `get_buffer_state`
- `read_property`
- `get_frame_requirements`
- `send_frame`

If `get_buffer_state` is provided, the host can pace point submissions against
the device's reported fill level.

If `get_buffer_state` is omitted, the host cannot maintain a specific
device-side buffer target. In that case it falls back to an automatic
point-ingester cadence derived from the current point rate and submits fixed
batches on that schedule.

If `properties` / `read_property` are omitted, the plugin exposes no
properties.

If you use the frame-ingester path, `get_frame_requirements` and `send_frame`
must be provided together.

## Host services

If you implement `create_backend()`, Libera passes a
`const libera_host_services_t*` into it. That host table is versioned
separately from the main plugin ABI and currently exposes:

- `log(level, message)`
- `record_latency(host_ctx, nanoseconds)`
- `report_error(host_ctx, code, label)`

`host_ctx` is the opaque token Libera passes into `connect_controller()`. Keep
that token on the controller side if you want to report transport latency or
device-specific errors back into the host later from `send_points()` or
`send_frame()`.

## Discovery and connect

`discover()` emits `libera_controller_info_t` structs. Each one contains:

- `id`
- `label`
- `max_point_rate`
- optional `usage_state`
- optional `network` info
- optional `connect_cookie`

The `connect_cookie` is important: the host copies it back into
`connect_controller()`, so a plugin can keep a tiny transport-specific token
from discovery time instead of re-looking everything up by string id.

If the plugin provides `rescan()`, Libera calls it before each `discover()`
pass so network or USB plugins can refresh cached state.

## Properties

Plugins do not need to implement both "list properties" and "get by key".

Instead they provide:

- a static property table: `properties` + `property_count`
- one reader callback: `read_property(controller, property_index, ...)`

The host handles:

- listing properties
- key lookup
- `get_property(key)`

That keeps property boilerplate small.

## Choosing a transport shape

Plugins should mirror the same two backend shapes that built-in controllers use:

- `Point-ingester`
  The transport wants "some more points now". Implement `send_points()`. The
  host asks Libera's shared scheduler for a point batch, converts it to
  `libera_point_t`, and forwards it to the plugin.
  Point-ingester plugins can run in two modes:
  with `get_buffer_state()`, the host tries to maintain a target buffer level;
  without it, the host just keeps points flowing automatically based on point
  rate.
- `Frame-ingester`
  The transport wants one whole frame submission at a time. Implement the
  paired `get_frame_requirements()` + `send_frame()` callbacks. The host asks Libera's
  shared scheduler for one frame, converts it to `libera_point_t`, and forwards
  it to the plugin.

If both shapes are present in one plugin, the host prefers the
frame-ingester path because it most closely matches a built-in frame backend.

## Readiness and backpressure

There is no separate generic `is_ready()` callback in the plugin ABI. The host
uses different readiness signals depending on the transport shape:

- `Point-ingester`
  Backpressure is reported indirectly through `get_buffer_state()`. The plugin
  reports `points_in_buffer` and `total_buffer_points`, and the host uses that
  telemetry to decide whether to submit more points now or wait a little longer.
  That is the mode to use when the host should actively maintain a device-side
  buffer level.
- `Frame-ingester`
  Readiness is reported explicitly through `get_frame_requirements()`. Return
  `LIBERA_OK` and fill out the requirements when the transport can accept one
  more frame now. Return `LIBERA_ERR_BUSY` when the transport is not ready yet;
  the host waits briefly and asks again.

  When the active content source is a live point callback, the host also keeps
  a shared virtual point backlog for the callback-to-frame adapter. That
  backlog includes points already accepted through successful `send_frame()`
  calls plus any extra points currently staged inside the adapter while it
  searches for natural frame boundaries. The host uses that shared backlog to
  decide how many more callback points to request, so frame-ingester plugins do
  not need to invent their own point-side pacing policy.

Two details are important:

- `send_points()` / `send_frame()` are submission calls, not readiness polls.
  A non-`LIBERA_OK` return from those callbacks is treated as a send failure,
  not as normal backpressure.
- If a point-ingester plugin omits `get_buffer_state()`, the host falls back to
  automatic point-rate-based feeding. In that mode the host does not try to
  maintain a specific device-side buffer level; it derives a fixed batch size
  from the current point rate and keeps submitting on its own cadence.
- A frame-ingester plugin does not need `get_buffer_state()` for that callback
  adaptation path to work. The host can synthesize a virtual backlog from
  accepted `send_frame()` submissions. If the plugin does expose
  `get_buffer_state()`, the host uses that as the transport-side truth and
  still adds the adapter's own staged points on top when reporting the shared
  buffered-point view.

## Helper utilities

`libera_plugin.h` also ships a few small helpers so plugins do not need to
rebuild the same safety boilerplate:

- `libera_controller_info_init()` to zero and populate discovery records
- `libera_controller_info_set_network()` to attach IP/port metadata
- `libera_controller_info_set_cookie()` to copy a small discovery token into
  `connect_cookie`
- `libera_frame_requirements_init()` to populate frame requirements
- `libera_copy_string()` for fixed-size string fields and property output
- `LIBERA_PLUGIN_EXPORT(pluginApi)` to export `libera_plugin_get_api()`

## Minimal example

In the examples below:

- `type_name` identifies the controller family or backend type
- `display_name` is the human-readable family name
- `id` is the stable identifier for one discovered device
- `label` is the human-readable per-device name

### Point-ingester

```c
typedef struct {
    VendorSdk* sdk;
} MyBackend;

typedef struct {
    VendorDevice* device;
    bool armed;
} MyController;

static void discover(void* rawBackend,
                     libera_emit_controller_fn emit,
                     void* ctx) {
    libera_controller_info_t info;
    libera_controller_info_init(&info,
                                "acme-usb-001",
                                "Acme USB DAC #1",
                                30000);
    emit(ctx, &info);
}

static void* connect_controller(void* rawBackend,
                                const libera_controller_info_t* info,
                                libera_host_ctx_t hostCtx) {
    MyBackend* backend = rawBackend;
    MyController* controller = calloc(1, sizeof(MyController));
    controller->device = vendor_open(backend->sdk, info->id);
    controller->armed = false;
    (void)hostCtx;
    return controller;
}

static void destroy_controller(void* rawController) {
    MyController* controller = rawController;
    vendor_close(controller->device);
    free(controller);
}

static libera_status_t send_points(void* rawController,
                                   const libera_point_t* points,
                                   uint32_t count) {
    MyController* controller = rawController;
    return vendor_send_points(controller->device, points, count)
        ? LIBERA_OK
        : LIBERA_ERR_INTERNAL;
}

static const libera_plugin_api_t pluginApi = {
    .abi_version = LIBERA_PLUGIN_API_VERSION,
    .type_name = "AcmeUsbDac",
    .display_name = "Acme USB DAC",
    .discover = discover,
    .connect_controller = connect_controller,
    .destroy_controller = destroy_controller,
    .send_points = send_points,
};

LIBERA_PLUGIN_EXPORT(pluginApi)
```

### Frame-ingester

A frame-ingester plugin replaces `send_points()` with the paired frame
callbacks:

```c
static libera_status_t get_frame_requirements(void* rawController,
                                              libera_frame_requirements_t* out) {
    MyController* controller = rawController;
    if (!controller || !out) {
        return LIBERA_ERR_INVALID_ARGUMENT;
    }

    libera_frame_requirements_init(out,
                                   /* maximumPointsRequired */ 1000,
                                   /* preferredPointCount   */ 1000,
                                   /* blankFramePointCount  */ 1000);
    out->estimated_first_point_render_delay_ns = 5ull * 1000ull * 1000ull;
    return LIBERA_OK;
}

static libera_status_t send_frame(void* rawController,
                                  const libera_point_t* points,
                                  uint32_t count) {
    MyController* controller = rawController;
    return vendor_send_frame(controller->device, points, count)
        ? LIBERA_OK
        : LIBERA_ERR_INTERNAL;
}
```

Then wire those fields into `libera_plugin_api_t`:

```c
static const libera_plugin_api_t pluginApi = {
    .abi_version = LIBERA_PLUGIN_API_VERSION,
    .type_name = "AcmeFrameDac",
    .display_name = "Acme Frame DAC",
    .discover = discover,
    .connect_controller = connect_controller,
    .destroy_controller = destroy_controller,
    .get_frame_requirements = get_frame_requirements,
    .send_frame = send_frame,
};
```

See the full working example here:

- [example_plugin.cpp](../examples/example_plugin.cpp)

## Lifecycle

For a point-ingester plugin that supports two devices, the call flow is:

```text
load .dylib
├── libera_plugin_get_api()
├── create_backend(host)                 optional
├── rescan(backend)                      optional
├── discover(backend, emit, ctx)         emits "dev-A", "dev-B"
│
├── connect_controller(backend, dev-A, host_ctx_A) -> controller_A
│   ├── set_point_rate(controller_A, 30000)         optional
│   ├── set_armed(controller_A, true)               optional
│   ├── get_buffer_state(controller_A, &bs)         optional
│   ├── send_points(controller_A, pts, n)           repeated
│   └── destroy_controller(controller_A)
│
├── connect_controller(backend, dev-B, host_ctx_B) -> controller_B
│   └── ...
│
└── destroy_backend(backend)             optional
```

For a frame-ingester plugin, the per-controller loop becomes:

```text
load .dylib
├── libera_plugin_get_api()
├── create_backend(host)                           optional
├── discover(backend, emit, ctx)
│
├── connect_controller(backend, dev-A, host_ctx_A) -> controller_A
│   ├── set_point_rate(controller_A, 30000)           optional
│   ├── set_armed(controller_A, true)                 optional
│   ├── get_frame_requirements(controller_A, &req)    repeated
│   ├── send_frame(controller_A, frame_pts, n)        repeated
│   └── destroy_controller(controller_A)
│
└── destroy_backend(backend)                       optional
```

`discover()` may be called repeatedly while the app is running.

## Streaming model

The host still owns the high-level streaming loop.

That means the plugin does **not** implement Libera's scheduler policy itself.
The plugin's job is just to:

- discover devices
- open/close controller connections
- describe what kind of transport payload it wants next
- accept that payload through `send_points()` or `send_frame()`
- optionally expose buffer state so the host can pace or report status

Whether the application is using queued frames or a live point callback, the
host adapts that content source into the payload shape the plugin asked for.

For frame-ingester plugins, that host-owned adaptation now also includes the
virtual point backlog used to throttle live callbacks. The plugin still only
needs to expose frame readiness and accept frames; the host keeps the callback
side from overfilling the transport by accounting for both accepted frames and
any points staged in the shared framer.

If your vendor SDK is internally frame-based, prefer the
`get_frame_requirements()` + `send_frame()` path so the plugin stays aligned
with built-in frame-ingester backends. Only re-buffer point batches inside the
plugin if you deliberately want the transport to behave like a point-ingester.

## Building

Typical commands:

```sh
# macOS
c++ -shared -fPIC -std=c++17 -o my-plugin.dylib my_plugin.cpp -I <path-to>/include

# Linux
c++ -shared -fPIC -std=c++17 -o my-plugin.so my_plugin.cpp -I <path-to>/include

# Windows (MSVC)
cl /LD /std:c++17 /I <path-to>\include my_plugin.cpp /Fe:my-plugin.dll
```
